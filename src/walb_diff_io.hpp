#pragma once

#include <vector>
#include <cassert>
#include "fileio.hpp"
#include "bdev_util.hpp"
#include "walb_diff_base.hpp"
#include "walb_diff_pack.hpp"
#include "discard_type.hpp"
#include "cybozu/exception.hpp"

namespace walb {

enum IoType
{
    Normal, Discard, Zero, Ignore,
};

IoType decideIoType(const DiffRecord& rec, DiscardType discardType);
void issueIo(cybozu::util::File& file, DiscardType discardType, const DiffRecord& rec, const char *iodata, AlignedArray& zero);
void issueDiffPack(cybozu::util::File& file, DiscardType discardType, MemoryDiffPack& pack, AlignedArray& zero);

} // namespace walb
