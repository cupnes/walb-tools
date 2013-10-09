/**
 * @file
 * @brief Verify a walb log by comparing with an IO recipe.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <string>
#include <cstdio>
#include <stdexcept>
#include <cstdint>
#include <queue>
#include <memory>
#include <deque>
#include <algorithm>
#include <utility>
#include <set>
#include <limits>

#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <getopt.h>

#include "stdout_logger.hpp"

#include "util.hpp"
#include "memory_buffer.hpp"
#include "fileio.hpp"
#include "walb_log_file.hpp"
#include "io_recipe.hpp"
#include "walb/common.h"
#include "walb/block_size.h"

/**
 * Command line configuration.
 */
class Config
{
private:
    bool isVerbose_;
    bool isHelp_;
    std::string wlogPath_; /* walb log path or "-" for stdin. */
    std::string recipePath_; /* recipe path or "-" for stdin. */
    std::vector<std::string> args_;

public:
    Config(int argc, char* argv[])
        : isVerbose_(false)
        , isHelp_(false)
        , wlogPath_("-")
        , recipePath_("-")
        , args_() {
        parse(argc, argv);
    }

    bool isVerbose() const { return isVerbose_; }
    bool isHelp() const { return isHelp_; }
    const std::string& recipePath() const { return recipePath_; }
    const std::string& wlogPath() const { return wlogPath_; }

    void print() const {
        FILE *fp = ::stderr;
        ::fprintf(fp, "verbose: %d\n"
                  "isHelp: %d\n"
                  "recipe: %s\n"
                  "wlog: %s\n",
                   isVerbose(), isHelp(),
                  recipePath_.c_str(), wlogPath().c_str());
        int i = 0;
        for (const auto& s : args_) {
            ::fprintf(fp, "arg%d: %s\n", i++, s.c_str());
        }
    }

    static void printHelp() {
        ::printf("%s", generateHelpString().c_str());
    }

    void check() const {
        if (recipePath_ == "-" && wlogPath_ == "-") {
            throw RT_ERR("Specify --recipe or --wlog.");
        }
    }

private:
    /* Option ids. */
    enum Opt {
        RECIPEPATH = 1,
        WLOGPATH,
        VERBOSE,
        HELP,
    };

    template <typename IntType>
    IntType str2int(const char *str) const {
        return static_cast<IntType>(cybozu::util::fromUnitIntString(str));
    }

    void parse(int argc, char* argv[]) {
        while (1) {
            const struct option long_options[] = {
                {"recipe", 1, 0, Opt::RECIPEPATH},
                {"wlog", 1, 0, Opt::WLOGPATH},
                {"verbose", 0, 0, Opt::VERBOSE},
                {"help", 0, 0, Opt::HELP},
                {0, 0, 0, 0}
            };
            int option_index = 0;
            int c = ::getopt_long(argc, argv, "r:w:vh", long_options, &option_index);
            if (c == -1) { break; }

            switch (c) {
            case Opt::RECIPEPATH:
            case 'r':
                recipePath_ = optarg;
                break;
            case Opt::WLOGPATH:
            case 'w':
                wlogPath_ = optarg;
                break;
            case Opt::VERBOSE:
            case 'v':
                isVerbose_ = true;
                break;
            case Opt::HELP:
            case 'h':
                isHelp_ = true;
                break;
            default:
                throw RT_ERR("Unknown option.");
            }
        }

        while(optind < argc) {
            args_.push_back(std::string(argv[optind++]));
        }
    }

    static std::string generateHelpString() {
        return cybozu::util::formatString(
            "verify_wlog: verify a walb log with an IO recipe.\n"
            "Usage: verify_wlog [options]\n"
            "Options:\n"
            "  -r, --recipe PATH: recipe file path. '-' for stdin. (default: '-')\n"
            "  -w, --wlog PATH:   wlog file path. '-' for stdin. (default: '-')\n"
            "  -v, --verbose:     verbose messages to stderr.\n"
            "  -h, --help:        show this message.\n");
    }
};

class WlogVerifier
{
private:
    using PackHeader = walb::log::PackHeaderRaw;
    using PackHeaderPtr = std::shared_ptr<PackHeader>;
    using PackIo = walb::log::PackIoRaw;

    const Config &config_;

public:
    WlogVerifier(const Config &config)
        : config_(config) {}

    void run() {
        /* Get IO recipe parser. */
        std::shared_ptr<cybozu::util::FileOpener> rFop;
        if (config_.recipePath() != "-") {
            rFop.reset(new cybozu::util::FileOpener(config_.recipePath(), O_RDONLY));
        }
        int rFd = 0;
        if (rFop) { rFd = rFop->fd(); }
        walb::util::IoRecipeParser recipeParser(rFd);

        /* Get wlog file descriptor. */
        std::shared_ptr<cybozu::util::FileOpener> wlFop;
        if (config_.wlogPath() != "-") {
            wlFop.reset(new cybozu::util::FileOpener(config_.wlogPath(), O_RDONLY));
        }
        int wlFd = 0;
        if (wlFop) { wlFd = wlFop->fd(); }
        cybozu::util::FdReader wlFdr(wlFd);

        /* Read wlog header. */
        walb::log::FileHeader wh;
        wh.read(wlFdr);
        if (!wh.isValid(true)) {
            throw RT_ERR("invalid wlog header.");
        }

        const unsigned int pbs = wh.pbs();
        const unsigned int bufferSize = 16 * 1024 * 1024;
        cybozu::util::BlockAllocator<uint8_t> ba(bufferSize / pbs, pbs, pbs);
        const uint32_t salt = wh.salt();

        uint64_t beginLsid = wh.beginLsid();
        uint64_t endLsid = wh.endLsid();
        uint64_t lsid = beginLsid;

        /* Read walb logs and verify them with IO recipes. */
        while (lsid < endLsid) {
            PackHeaderPtr loghp = readPackHeader(wlFdr, ba, salt);
            PackHeader &logh = *loghp;
            if (lsid != logh.logpackLsid()) { throw RT_ERR("wrong lsid"); }
            std::queue<PackIo> q;
            readPackIo(logh, wlFdr, ba, q);

            while (!q.empty()) {
                PackIo packIo = std::move(q.front());
                q.pop();
                if (recipeParser.isEnd()) {
                    throw RT_ERR("Recipe not found.");
                }
                walb::util::IoRecipe recipe = recipeParser.get();
                if (recipe.offsetB() != packIo.record().offset()) {
                    RT_ERR("offset mismatch.");
                }
                if (recipe.ioSizeB() != packIo.record().ioSizeLb()) {
                    RT_ERR("io_size mismatch.");
                }
                /* Validate the log and confirm checksum equality. */
                const uint32_t csum0 = packIo.calcIoChecksum(0);
                const uint32_t csum1 = packIo.record().checksum();
                const uint32_t csum2 = packIo.calcIoChecksum();
                const bool isValid = packIo.isValid(false) &&
                    recipe.csum() == csum0 && csum1 == csum2;

                /* Print result. */
                ::printf("%s\t%s\t%08x\t%08x\t%08x\n", isValid ? "OK" : "NG",
                         recipe.toString().c_str(), csum0, csum1, csum2);
            }

            lsid = logh.nextLogpackLsid();
        }

        if (!recipeParser.isEnd()) {
            throw RT_ERR("There are still remaining recipes.");
        }
    }

private:
    using Block = std::shared_ptr<uint8_t>;

    Block readBlock(
        cybozu::util::FdReader &fdr, cybozu::util::BlockAllocator<u8> &ba) {
        Block b = ba.alloc();
        unsigned int bs = ba.blockSize();
        fdr.read(reinterpret_cast<char *>(b.get()), bs);
        return b;
    }

    PackHeaderPtr readPackHeader(
        cybozu::util::FdReader &fdr, cybozu::util::BlockAllocator<u8> &ba, uint32_t salt) {
        Block b = readBlock(fdr, ba);
        return PackHeaderPtr(new PackHeader(b, ba.blockSize(), salt));
    }

    void readPackIo(
        PackHeader &logh, cybozu::util::FdReader &fdr,
        cybozu::util::BlockAllocator<u8> &ba, std::queue<PackIo> &queue) {
        for (size_t i = 0; i < logh.nRecords(); i++) {
            PackIo packIo(logh, i);
            walb::log::RecordRaw &rec = packIo.record();
            if (!rec.hasData()) { continue; }
            for (size_t j = 0; j < rec.ioSizePb(); j++) {
                packIo.blockData().addBlock(readBlock(fdr, ba));
            }
            if (!rec.hasDataForChecksum()) { continue; }
            /* Only normal IOs will be inserted. */
            queue.push(std::move(packIo));
        }
    }
};

int main(int argc, char* argv[])
{
    try {
        Config config(argc, argv);
        /* config.print(); */
        if (config.isHelp()) {
            Config::printHelp();
            return 0;
        }
        config.check();

        WlogVerifier v(config);
        v.run();
        return 0;
    } catch (std::exception& e) {
        LOGe("Exception: %s\n", e.what());
    } catch (...) {
        LOGe("Caught other error.\n");
    }
    return 1;
}

/* end file. */
