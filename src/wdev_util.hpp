#pragma once

#include "cybozu/exception.hpp"
#include "cybozu/string_operation.hpp"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "util.hpp"
#include "fileio.hpp"
#include "file_path.hpp"
#include "process.hpp"
#include "fdstream.hpp"
#include "walb/ioctl.h"
#include "walb/block_size.h"

namespace walb {
namespace device {

static const std::string WDEV_PATH_PREFIX = "/dev/walb/";

namespace local {

inline void invokeWdevIoctl(const std::string& wdevPath, struct walb_ctl *ctl, int openFlag)
{
    const char *const FUNC = __func__;
    cybozu::util::FileOpener f(wdevPath, openFlag);
    int ret = ::ioctl(f.fd(), WALB_IOCTL_WDEV, ctl);
    if (ret < 0) {
        throw cybozu::Exception(FUNC) << "ioctl error" << cybozu::ErrorNo();
    }
}

/**
 * Get a lsid of the volume using ioctl.
 *
 * @command
 *   WALB_IOCTL_GET_XXX_LSID defined walb/ioctl.h.
 *   XXX: OLDEST, PERMANENT, WRITTEN, PERMANENT, COMPLETED.
 */
inline uint64_t getLsid(const std::string& wdevPath, int command)
{
    struct walb_ctl ctl;
    ::memset(&ctl, 0, sizeof(ctl));
    ctl.command = command;

    local::invokeWdevIoctl(wdevPath, &ctl, O_RDWR);
    if (ctl.val_u64 == uint64_t(-1)) {
        throw cybozu::Exception("getLsid:invalid lsid");
    }
    return ctl.val_u64;
}

inline void setOldestLsid(const std::string& wdevPath, uint64_t lsid)
{
    struct walb_ctl ctl;
    ::memset(&ctl, 0, sizeof(ctl));
    ctl.command = WALB_IOCTL_SET_OLDEST_LSID;
    ctl.val_u64 = lsid;

    local::invokeWdevIoctl(wdevPath, &ctl, O_RDWR);
}

/**
 * Parse "XXX:YYY" string where XXX is major id and YYY is minor id.
 */
inline std::pair<uint32_t, uint32_t> parseDeviceIdStr(const std::string& devIdStr)
{
    const char *const FUNC = __func__;
    std::vector<std::string> v = cybozu::Split(devIdStr, ':', 2);
    if (v.size() != 2) {
        throw cybozu::Exception(FUNC) << "parse error" << devIdStr;
    }
    const uint32_t major = cybozu::atoi(v[0]);
    const uint32_t minor = cybozu::atoi(v[1]);
    return std::make_pair(major, minor);
}

/**
 * Replace charactor x to y in a string.
 */
inline void replaceChar(std::string &s, const char x, const char y)
{
    for (;;) {
        size_t n = s.find(x);
        if (n == std::string::npos) break;
        s[n] = y;
    }
}

/**
 * Get block device path from major and minor id using lsblk command.
 */
inline std::string getDevPathFromId(uint32_t major, uint32_t minor)
{
    const char *const FUNC = __func__;
    const std::string res = cybozu::process::call(
        "/bin/lsblk", { "-l", "-n", "-r", "-o", "KNAME,MAJ:MIN" });
    for (const std::string& line : cybozu::Split(res, '\n')) {
        const std::vector<std::string> v = cybozu::Split(line, ' ');
        if (v.size() != 2) {
            throw cybozu::Exception(FUNC) << "lsblk output parse error" << line;
        }
        std::string name = v[0];
        uint32_t majorX, minorX;
        std::tie(majorX, minorX) = local::parseDeviceIdStr(v[1]);
        if (major != majorX || minor != minorX) continue;
        replaceChar(name, '!', '/');
        cybozu::FilePath path("/dev");
        path += name;
        if (!path.stat().exists()) {
            throw cybozu::Exception(FUNC) << "not exists" << path.str();
        }
        return path.str();
    }
    throw cybozu::Exception(FUNC) << "not found" << major << minor;
}

inline cybozu::FilePath getSysfsPath(const std::string& wdevName)
{
    return cybozu::FilePath(cybozu::util::formatString("/sys/block/walb!%s", wdevName.c_str()));
}

inline std::string readOneLine(const std::string& path)
{
    cybozu::util::FileOpener fo(path, O_RDONLY);
    cybozu::ifdstream is(fo.fd());
    std::string line;
    is >> line;
    return line;
}

inline std::string getUnderlyingDevPath(const std::string& wdevName, bool isLog)
{
    cybozu::FilePath path =
        local::getSysfsPath(wdevName) + "walb" + (isLog ? "ldev" : "ddev");
    uint32_t major, minor;
    std::tie(major, minor) = local::parseDeviceIdStr(local::readOneLine(path.str()));
    return local::getDevPathFromId(major, minor);
}

} // namespace local

inline void resetWal(const std::string& wdevPath)
{
    struct walb_ctl ctl;
    ::memset(&ctl, 0, sizeof(ctl));
    ctl.command = WALB_IOCTL_CLEAR_LOG;

    local::invokeWdevIoctl(wdevPath, &ctl, O_RDWR);
}

inline uint64_t getPermanentLsid(const std::string& wdevPath)
{
    return local::getLsid(wdevPath, WALB_IOCTL_GET_PERMANENT_LSID);
}

inline uint64_t getOldestLsid(const std::string& wdevPath)
{
    return local::getLsid(wdevPath, WALB_IOCTL_GET_OLDEST_LSID);
}

inline bool isOverflow(const std::string& wdevPath)
{
    struct walb_ctl ctl;
    ::memset(&ctl, 0, sizeof(ctl));
    ctl.command = WALB_IOCTL_IS_LOG_OVERFLOW;

    local::invokeWdevIoctl(wdevPath, &ctl, O_RDWR);
    return ctl.val_int != 0;
}

inline void eraseWal(const std::string& wdevPath)
{
    if (isOverflow(wdevPath)) {
        throw cybozu::Exception("eraseWal") << "overflow" << wdevPath;
    }
    uint64_t lsid = getPermanentLsid(wdevPath);
    local::setOldestLsid(wdevPath, lsid);
}

/**
 * @sizeLb
 *   0 can be specified (auto-detect).
 */
inline void resize(const std::string& wdevPath, uint64_t sizeLb = 0)
{
    struct walb_ctl ctl;
    ::memset(&ctl, 0, sizeof(ctl));
    ctl.command = WALB_IOCTL_RESIZE;
    ctl.val_u64 = sizeLb;

    local::invokeWdevIoctl(wdevPath, &ctl, O_RDWR);
}

inline uint64_t getSizeLb(const std::string& wdevPath)
{
    cybozu::util::FileOpener f(wdevPath, O_RDONLY);
    uint64_t size;
    if (::ioctl(f.fd(), BLKGETSIZE64, &size) < 0) {
        throw cybozu::Exception("getSizeLb:bad ioctl") << cybozu::ErrorNo();
    }
    return size / LOGICAL_BLOCK_SIZE;
}

/**
 * Get polling path.
 *
 * @wdevName walb device name.
 * RETURN:
 *   full path of polling target.
 */
inline std::string getPollingPath(const std::string &wdevName)
{
    return (local::getSysfsPath(wdevName) + "walb" + "lsids").str();
}

inline std::string getUnderlyingLogDevPath(const std::string& wdevName)
{
    return local::getUnderlyingDevPath(wdevName, true);
}

inline std::string getUnderlyingDataDevPath(const std::string& wdevName)
{
    return local::getUnderlyingDevPath(wdevName, false);
}

inline std::string getWdevPathFromWdevName(const std::string& wdevName)
{
    return WDEV_PATH_PREFIX + wdevName;
}

inline std::string getWldevPathFromWdevName(const std::string& wdevName)
{
    return WDEV_PATH_PREFIX + "L" + wdevName;
}

inline std::string getWdevNameFromWdevPath(const std::string& wdevPath)
{
    const char *const FUNC = __func__;
    if (wdevPath.find(WDEV_PATH_PREFIX) != 0) {
        throw cybozu::Exception(FUNC) << "bad name" << wdevPath;
    }
    return wdevPath.substr(WDEV_PATH_PREFIX.size());
}

}} // walb::device

