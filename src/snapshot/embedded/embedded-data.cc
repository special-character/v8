// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/snapshot/embedded/embedded-data.h"

#include "src/codegen/assembler-inl.h"
#include "src/codegen/callable.h"
#include "src/objects/objects-inl.h"
#include "src/snapshot/snapshot-utils.h"
#include "src/snapshot/snapshot.h"

namespace v8 {
namespace internal {

// static
bool InstructionStream::PcIsOffHeap(Isolate* isolate, Address pc) {
  const Address start =
      reinterpret_cast<Address>(isolate->embedded_blob_code());
  return start <= pc && pc < start + isolate->embedded_blob_code_size();
}

// static
Code InstructionStream::TryLookupCode(Isolate* isolate, Address address) {
  if (!PcIsOffHeap(isolate, address)) return Code();

  EmbeddedData d = EmbeddedData::FromBlob();
  if (address < d.InstructionStartOfBuiltin(0)) return Code();

  // Note: Addresses within the padding section between builtins (i.e. within
  // start + size <= address < start + padded_size) are interpreted as belonging
  // to the preceding builtin.

  int l = 0, r = Builtins::builtin_count;
  while (l < r) {
    const int mid = (l + r) / 2;
    Address start = d.InstructionStartOfBuiltin(mid);
    Address end = start + d.PaddedInstructionSizeOfBuiltin(mid);

    if (address < start) {
      r = mid;
    } else if (address >= end) {
      l = mid + 1;
    } else {
      return isolate->builtins()->builtin(mid);
    }
  }

  UNREACHABLE();
}

// static
void InstructionStream::CreateOffHeapInstructionStream(
    Isolate* isolate, uint8_t** code, uint32_t* code_size, uint8_t** metadata,
    uint32_t* metadata_size) {
  // Create the embedded blob from scratch using the current Isolate's heap.
  EmbeddedData d = EmbeddedData::FromIsolate(isolate);

  // Allocate the backing store that will contain the embedded blob in this
  // Isolate. The backing store is on the native heap, *not* on V8's garbage-
  // collected heap.
  v8::PageAllocator* page_allocator = v8::internal::GetPlatformPageAllocator();
  const uint32_t alignment =
      static_cast<uint32_t>(page_allocator->AllocatePageSize());

  void* const requested_allocation_code_address =
      AlignedAddress(isolate->heap()->GetRandomMmapAddr(), alignment);
  const uint32_t allocation_code_size = RoundUp(d.code_size(), alignment);
  uint8_t* allocated_code_bytes = static_cast<uint8_t*>(AllocatePages(
      page_allocator, requested_allocation_code_address, allocation_code_size,
      alignment, PageAllocator::kReadWrite));
  CHECK_NOT_NULL(allocated_code_bytes);

  void* const requested_allocation_metadata_address =
      AlignedAddress(isolate->heap()->GetRandomMmapAddr(), alignment);
  const uint32_t allocation_metadata_size =
      RoundUp(d.metadata_size(), alignment);
  uint8_t* allocated_metadata_bytes = static_cast<uint8_t*>(AllocatePages(
      page_allocator, requested_allocation_metadata_address,
      allocation_metadata_size, alignment, PageAllocator::kReadWrite));
  CHECK_NOT_NULL(allocated_metadata_bytes);

  // Copy the embedded blob into the newly allocated backing store. Switch
  // permissions to read-execute since builtin code is immutable from now on
  // and must be executable in case any JS execution is triggered.
  //
  // Once this backing store is set as the current_embedded_blob, V8 cannot tell
  // the difference between a 'real' embedded build (where the blob is embedded
  // in the binary) and what we are currently setting up here (where the blob is
  // on the native heap).
  std::memcpy(allocated_code_bytes, d.code(), d.code_size());
  CHECK(SetPermissions(page_allocator, allocated_code_bytes,
                       allocation_code_size, PageAllocator::kReadExecute));

  std::memcpy(allocated_metadata_bytes, d.metadata(), d.metadata_size());
  CHECK(SetPermissions(page_allocator, allocated_metadata_bytes,
                       allocation_metadata_size, PageAllocator::kRead));

  *code = allocated_code_bytes;
  *code_size = d.code_size();
  *metadata = allocated_metadata_bytes;
  *metadata_size = d.metadata_size();

  d.Dispose();
}

// static
void InstructionStream::FreeOffHeapInstructionStream(uint8_t* code,
                                                     uint32_t code_size,
                                                     uint8_t* metadata,
                                                     uint32_t metadata_size) {
  v8::PageAllocator* page_allocator = v8::internal::GetPlatformPageAllocator();
  const uint32_t page_size =
      static_cast<uint32_t>(page_allocator->AllocatePageSize());
  CHECK(FreePages(page_allocator, code, RoundUp(code_size, page_size)));
  CHECK(FreePages(page_allocator, metadata, RoundUp(metadata_size, page_size)));
}

namespace {

bool BuiltinAliasesOffHeapTrampolineRegister(Isolate* isolate, Code code) {
  DCHECK(Builtins::IsIsolateIndependent(code.builtin_index()));
  switch (Builtins::KindOf(code.builtin_index())) {
    case Builtins::CPP:
    case Builtins::TFC:
    case Builtins::TFH:
    case Builtins::TFJ:
    case Builtins::TFS:
      break;

    // Bytecode handlers will only ever be used by the interpreter and so there
    // will never be a need to use trampolines with them.
    case Builtins::BCH:
    case Builtins::ASM:
      // TODO(jgruber): Extend checks to remaining kinds.
      return false;
  }

  Callable callable = Builtins::CallableFor(
      isolate, static_cast<Builtins::Name>(code.builtin_index()));
  CallInterfaceDescriptor descriptor = callable.descriptor();

  if (descriptor.ContextRegister() == kOffHeapTrampolineRegister) {
    return true;
  }

  for (int i = 0; i < descriptor.GetRegisterParameterCount(); i++) {
    Register reg = descriptor.GetRegisterParameter(i);
    if (reg == kOffHeapTrampolineRegister) return true;
  }

  return false;
}

void FinalizeEmbeddedCodeTargets(Isolate* isolate, EmbeddedData* blob) {
  static const int kRelocMask =
      RelocInfo::ModeMask(RelocInfo::CODE_TARGET) |
      RelocInfo::ModeMask(RelocInfo::RELATIVE_CODE_TARGET);

  STATIC_ASSERT(Builtins::kAllBuiltinsAreIsolateIndependent);
  for (int i = 0; i < Builtins::builtin_count; i++) {
    Code code = isolate->builtins()->builtin(i);
    RelocIterator on_heap_it(code, kRelocMask);
    RelocIterator off_heap_it(blob, code, kRelocMask);

#if defined(V8_TARGET_ARCH_X64) || defined(V8_TARGET_ARCH_ARM64) || \
    defined(V8_TARGET_ARCH_ARM) || defined(V8_TARGET_ARCH_MIPS) ||  \
    defined(V8_TARGET_ARCH_IA32) || defined(V8_TARGET_ARCH_S390)
    // On these platforms we emit relative builtin-to-builtin
    // jumps for isolate independent builtins in the snapshot. This fixes up the
    // relative jumps to the right offsets in the snapshot.
    // See also: Code::IsIsolateIndependent.
    while (!on_heap_it.done()) {
      DCHECK(!off_heap_it.done());

      RelocInfo* rinfo = on_heap_it.rinfo();
      DCHECK_EQ(rinfo->rmode(), off_heap_it.rinfo()->rmode());
      Code target = Code::GetCodeFromTargetAddress(rinfo->target_address());
      CHECK(Builtins::IsIsolateIndependentBuiltin(target));

      // Do not emit write-barrier for off-heap writes.
      off_heap_it.rinfo()->set_target_address(
          blob->InstructionStartOfBuiltin(target.builtin_index()),
          SKIP_WRITE_BARRIER);

      on_heap_it.next();
      off_heap_it.next();
    }
    DCHECK(off_heap_it.done());
#else
    // Architectures other than x64 and arm/arm64 do not use pc-relative calls
    // and thus must not contain embedded code targets. Instead, we use an
    // indirection through the root register.
    CHECK(on_heap_it.done());
    CHECK(off_heap_it.done());
#endif  // defined(V8_TARGET_ARCH_X64) || defined(V8_TARGET_ARCH_ARM64)
  }
}

}  // namespace

// static
EmbeddedData EmbeddedData::FromIsolate(Isolate* isolate) {
  Builtins* builtins = isolate->builtins();

  // Store instruction stream lengths and offsets.
  std::vector<struct Metadata> metadata(kTableSize);

  bool saw_unsafe_builtin = false;
  uint32_t raw_code_size = 0;
  STATIC_ASSERT(Builtins::kAllBuiltinsAreIsolateIndependent);
  for (int i = 0; i < Builtins::builtin_count; i++) {
    Code code = builtins->builtin(i);

    // Sanity-check that the given builtin is isolate-independent and does not
    // use the trampoline register in its calling convention.
    if (!code.IsIsolateIndependent(isolate)) {
      saw_unsafe_builtin = true;
      fprintf(stderr, "%s is not isolate-independent.\n", Builtins::name(i));
    }
    if (BuiltinAliasesOffHeapTrampolineRegister(isolate, code)) {
      saw_unsafe_builtin = true;
      fprintf(stderr, "%s aliases the off-heap trampoline register.\n",
              Builtins::name(i));
    }

    uint32_t length = static_cast<uint32_t>(code.raw_instruction_size());

    DCHECK_EQ(0, raw_code_size % kCodeAlignment);
    metadata[i].instructions_offset = raw_code_size;
    metadata[i].instructions_length = length;

    // Align the start of each instruction stream.
    raw_code_size += PadAndAlign(length);
  }
  CHECK_WITH_MSG(
      !saw_unsafe_builtin,
      "One or more builtins marked as isolate-independent either contains "
      "isolate-dependent code or aliases the off-heap trampoline register. "
      "If in doubt, ask jgruber@");

  const uint32_t blob_code_size = RawCodeOffset() + raw_code_size;
  uint8_t* const blob_code = new uint8_t[blob_code_size];
  uint8_t* const raw_code_start = blob_code + RawCodeOffset();
  const uint32_t blob_metadata_size =
      MetadataTableOffset() + MetadataTableSize();
  uint8_t* const blob_metadata = new uint8_t[blob_metadata_size];

  // Initially zap the entire blob, effectively padding the alignment area
  // between two builtins with int3's (on x64/ia32).
  ZapCode(reinterpret_cast<Address>(blob_code), blob_code_size);

  // Hash relevant parts of the Isolate's heap and store the result.
  {
    STATIC_ASSERT(IsolateHashSize() == kSizetSize);
    const size_t hash = isolate->HashIsolateForEmbeddedBlob();
    std::memcpy(blob_metadata + IsolateHashOffset(), &hash, IsolateHashSize());
  }

  // Write the metadata tables.
  DCHECK_EQ(MetadataTableSize(), sizeof(metadata[0]) * metadata.size());
  std::memcpy(blob_metadata + MetadataTableOffset(), metadata.data(),
              MetadataTableSize());

  // Write the raw data section.
  STATIC_ASSERT(Builtins::kAllBuiltinsAreIsolateIndependent);
  for (int i = 0; i < Builtins::builtin_count; i++) {
    Code code = builtins->builtin(i);
    uint32_t offset = metadata[i].instructions_offset;
    uint8_t* dst = raw_code_start + offset;
    DCHECK_LE(RawCodeOffset() + offset + code.raw_instruction_size(),
              blob_code_size);
    std::memcpy(dst, reinterpret_cast<uint8_t*>(code.raw_instruction_start()),
                code.raw_instruction_size());
  }

  EmbeddedData d(blob_code, blob_code_size, blob_metadata, blob_metadata_size);

  // Fix up call targets that point to other embedded builtins.
  FinalizeEmbeddedCodeTargets(isolate, &d);

  // Hash the blob and store the result.
  {
    STATIC_ASSERT(EmbeddedBlobHashSize() == kSizetSize);
    const size_t hash = d.CreateEmbeddedBlobHash();
    std::memcpy(blob_metadata + EmbeddedBlobHashOffset(), &hash,
                EmbeddedBlobHashSize());

    DCHECK_EQ(hash, d.CreateEmbeddedBlobHash());
    DCHECK_EQ(hash, d.EmbeddedBlobHash());
  }

  if (FLAG_serialization_statistics) d.PrintStatistics();

  return d;
}

Address EmbeddedData::InstructionStartOfBuiltin(int i) const {
  DCHECK(Builtins::IsBuiltinId(i));
  const struct Metadata* metadata = Metadata();
  const uint8_t* result = RawCode() + metadata[i].instructions_offset;
  DCHECK_LE(result, code_ + code_size_);
  DCHECK_IMPLIES(result == code_ + code_size_,
                 InstructionSizeOfBuiltin(i) == 0);
  return reinterpret_cast<Address>(result);
}

uint32_t EmbeddedData::InstructionSizeOfBuiltin(int i) const {
  DCHECK(Builtins::IsBuiltinId(i));
  const struct Metadata* metadata = Metadata();
  return metadata[i].instructions_length;
}

Address EmbeddedData::InstructionStartOfBytecodeHandlers() const {
  return InstructionStartOfBuiltin(Builtins::kFirstBytecodeHandler);
}

Address EmbeddedData::InstructionEndOfBytecodeHandlers() const {
  STATIC_ASSERT(Builtins::kFirstBytecodeHandler + kNumberOfBytecodeHandlers +
                    2 * kNumberOfWideBytecodeHandlers ==
                Builtins::builtin_count);
  int lastBytecodeHandler = Builtins::builtin_count - 1;
  return InstructionStartOfBuiltin(lastBytecodeHandler) +
         InstructionSizeOfBuiltin(lastBytecodeHandler);
}

size_t EmbeddedData::CreateEmbeddedBlobHash() const {
  STATIC_ASSERT(EmbeddedBlobHashOffset() == 0);
  STATIC_ASSERT(EmbeddedBlobHashSize() == kSizetSize);
  // Hash the entire blob except the hash field itself.
  Vector<const byte> payload1(metadata_ + EmbeddedBlobHashSize(),
                              metadata_size_ - EmbeddedBlobHashSize());
  Vector<const byte> payload2(code_, code_size_);
  return Checksum(payload1, payload2);
}

void EmbeddedData::PrintStatistics() const {
  DCHECK(FLAG_serialization_statistics);

  constexpr int kCount = Builtins::builtin_count;

  int embedded_count = 0;
  int instruction_size = 0;
  int sizes[kCount];
  STATIC_ASSERT(Builtins::kAllBuiltinsAreIsolateIndependent);
  for (int i = 0; i < kCount; i++) {
    const int size = InstructionSizeOfBuiltin(i);
    instruction_size += size;
    sizes[embedded_count] = size;
    embedded_count++;
  }

  // Sort for percentiles.
  std::sort(&sizes[0], &sizes[embedded_count]);

  const int k50th = embedded_count * 0.5;
  const int k75th = embedded_count * 0.75;
  const int k90th = embedded_count * 0.90;
  const int k99th = embedded_count * 0.99;

  PrintF("EmbeddedData:\n");
  PrintF("  Total size:                         %d\n",
         static_cast<int>(code_size() + metadata_size()));
  PrintF("  Metadata size:                      %d\n",
         static_cast<int>(metadata_size()));
  PrintF("  Instruction size:                   %d\n", instruction_size);
  PrintF("  Padding:                            %d\n",
         static_cast<int>(code_size() - instruction_size));
  PrintF("  Embedded builtin count:             %d\n", embedded_count);
  PrintF("  Instruction size (50th percentile): %d\n", sizes[k50th]);
  PrintF("  Instruction size (75th percentile): %d\n", sizes[k75th]);
  PrintF("  Instruction size (90th percentile): %d\n", sizes[k90th]);
  PrintF("  Instruction size (99th percentile): %d\n", sizes[k99th]);
  PrintF("\n");
}

}  // namespace internal
}  // namespace v8
