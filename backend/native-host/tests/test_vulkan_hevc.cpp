/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

// The Vulkan Video encoder and its pixel proof (plan Phase 13, C13.5) — Bruno's
// rule on the hardware (28/09/2026): where the driver encodes reliably the
// proof passes and the chain may be taken; where it does not — a driver that
// shows no encoder, or one that codes something other than its SPS says (the
// transform depth of bench §8o.3, replayed by MW_VK_ENCODE_DEPTH's witness) —
// the verdict is a refusal that names why, and VA-API encodes.
//
// Real GPU: the hardware half is skipped, and said, where the driver shows no
// Vulkan Video encoder — the UM790Pro's own Mesa 23.2, which is then checked
// to be refused by name. Run as root with VK_DRIVER_FILES pointing at a RADV
// that has one (a binary with file capabilities never sees the variable).

#include "native_test_framework.h"

#if defined(MW_NATIVE_LINUX_VULKAN)
#include "encode/HevcSliceParser.h"
#include "encode/linux/VulkanHevcDecoder.h"
#include "encode/linux/VulkanHevcEncoder.h"
#include "encode/linux/VulkanHevcProof.h"
#include "platform/linux/vulkan/VulkanDevice.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#endif

#include <cstdio>
#include <string>
#include <vector>

#if defined(MW_NATIVE_LINUX_VULKAN)
namespace {

using namespace mw::native;

/// The first render node whose driver Vulkan identifies.
bool findRenderNode(std::string& node, vulkan::DeviceIdentity& id)
{
    for (int minor = 128; minor < 136; ++minor) {
        const std::string path = "/dev/dri/renderD" + std::to_string(minor);
        std::string error;
        if (vulkan::VulkanDevice::identify(path, id, error)) {
            node = path;
            return true;
        }
    }
    return false;
}

bool contains(const std::string& text, const char* piece)
{
    return text.find(piece) != std::string::npos;
}

} // namespace
#endif

void run_vulkan_hevc_tests()
{
#if !defined(MW_NATIVE_LINUX_VULKAN)
    SECTION("Vulkan Video — HEVC through Vulkan and its pixel proof");
    std::fprintf(stderr, "  skipped: the Vulkan chain is not built\n");
#else
    SECTION("VulkanHevcProof — the comparison: equal pictures, a wrong CTB row, noise");
    {
        const int w = 128, h = 128;
        std::vector<uint8_t> a(static_cast<size_t>(w) * h * 3 / 2);
        for (size_t i = 0; i < a.size(); ++i)
            a[i] = static_cast<uint8_t>(16 + (i * 7) % 200);
        std::vector<uint8_t> b = a;
        encode::PicturePsnr q = encode::comparePictures(a.data(), b.data(), w, h, 64);
        CHECK(q.luma >= 99.0);
        CHECK(q.worstBand >= 99.0);
        CHECK(q.chroma >= 99.0);
        // One CTB row gone black: the band drops, the picture half as much.
        for (int row = 64; row < 128; ++row)
            for (int col = 0; col < w; ++col)
                b[static_cast<size_t>(row) * w + col] = 0;
        q = encode::comparePictures(a.data(), b.data(), w, h, 64);
        CHECK(q.worstBand < 10.0);
        CHECK(q.luma > q.worstBand);
        // ±2 everywhere: about 42 dB.
        b = a;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
            b[i] = static_cast<uint8_t>(a[i] + ((i & 1) ? 2 : -2));
        q = encode::comparePictures(a.data(), b.data(), w, h, 64);
        CHECK(q.luma > 41.0 && q.luma < 43.0);
    }

    SECTION("Vulkan Video — the driver says whether it has an encoder, and a refusal says why");
    std::string node;
    vulkan::DeviceIdentity id;
    if (!findRenderNode(node, id)) {
        std::fprintf(stderr, "  skipped: no Vulkan device behind any render node\n");
        return;
    }
    std::fprintf(stderr, "  %s: %s, encoder %s, decoder %s\n", node.c_str(), id.name.c_str(),
                 id.encodesHevc ? "yes" : "no", id.decodesHevc ? "yes" : "no");
    ::setenv("MW_VK_PROOF_CACHE", "0", 1);
    EncoderTuning tuning;
    if (!id.encodesHevc) {
        // The UM790Pro's Mesa 23.2: nothing to prove, and the verdict says so
        // without opening a device.
        const encode::VulkanHevcProof proof =
            encode::vulkanHevcVerdict(node, 1920, 1080, 60, tuning);
        CHECK(!proof.ran);
        CHECK(!proof.passed);
        CHECK(contains(proof.summary, "shows no Vulkan Video HEVC encoder"));
        std::fprintf(stderr, "  refused: %s\n", proof.summary.c_str());
        std::fprintf(stderr, "  skipped: the encoder itself (none on this driver)\n");
        ::unsetenv("MW_VK_PROOF_CACHE");
        return;
    }

    SECTION("VulkanHevcEncoder — the full transform depth, the driver's parameter sets, an IDR "
            "that carries them, no filler out");
    {
        vulkan::DeviceOptions options;
        options.wantHigh = false;
        options.encodeHevc = true;
        std::string error;
        std::shared_ptr<vulkan::VulkanDevice> device =
            vulkan::VulkanDevice::open(node, options, error);
        CHECK(device != nullptr);
        if (device) {
            encode::VulkanHevcEncoder encoder;
            const bool ok = encoder.init(device, Codec::Hevc, 1920, 1080, 60, 20000,
                                         /*intraRefresh=*/false, tuning, error);
            if (!ok) std::fprintf(stderr, "  init: %s\n", error.c_str());
            CHECK(ok);
            if (ok) {
                const encode::HevcSpsFields& sps = encoder.sps();
                CHECK_EQ(sps.width, 1920u);
                CHECK_EQ(sps.height % (1u << sps.log2CodingTreeBlock), 0u);
                CHECK_EQ(sps.cropBottom * 2, sps.height - 1080u);
                // CtbLog2SizeY − MinTbLog2SizeY: what AMD's firmware codes.
                CHECK_EQ(
                    sps.maxTransformHierarchyDepthInter,
                    static_cast<uint32_t>(sps.log2CodingTreeBlock - sps.log2MinTransformBlock));
                CHECK(!encoder.parameterSets().empty());
                CHECK(encoder.input().image != VK_NULL_HANDLE);
                CHECK_EQ(encoder.input().width, 1920);
                std::vector<uint8_t> grey(1920 * 1080 * 3 / 2, 128);
                CHECK(encoder.upload(grey.data(), error));
                encode::EncoderOutput out;
                CHECK(encoder.encode(false, 0, out, error));
                CHECK(out.keyframe);
                // The IDR goes out with the VPS in front.
                const std::vector<encode::HevcNalUnit> units =
                    encode::hevcNalUnits(out.data, out.size);
                CHECK(!units.empty() && units.front().type() == 32);
                encoder.releaseOutput();
                CHECK(encoder.encode(false, 1, out, error));
                CHECK(!out.keyframe);
                // The same grey again, a picture RADV's CBR pads up to its
                // whole budget: none of the filler goes out (stripHevcFiller).
                bool filler = false;
                for (const encode::HevcNalUnit& u : encode::hevcNalUnits(out.data, out.size))
                    filler = filler || u.type() == 38;
                CHECK(!filler);
                std::fprintf(stderr, "  an unchanged picture: %zu bytes out\n", out.size);
                encoder.releaseOutput();
                CHECK(encoder.supportsReferenceInvalidation());
                CHECK(encoder.invalidateReference(1, error));
                CHECK(encoder.encode(false, 2, out, error));
                CHECK(!out.keyframe); // healed by a delta from frame 0
                encoder.releaseOutput();
                CHECK(encoder.setBitrate(10000, error));
                CHECK(encoder.encode(false, 3, out, error));
                encoder.releaseOutput();
                std::fprintf(stderr, "  %s, %zu bytes of parameter sets\n",
                             encoder.describe().c_str(), encoder.parameterSets().size());
                CHECK(!encoder.intraRefreshEnabled()); // not asked
            }
        }
    }

    SECTION("VulkanHevcEncoder — intra refresh: the stream's sweeps, and a repair in one");
    {
        vulkan::DeviceOptions options;
        options.wantHigh = false;
        options.encodeHevc = true;
        std::string error;
        std::shared_ptr<vulkan::VulkanDevice> device =
            vulkan::VulkanDevice::open(node, options, error);
        CHECK(device != nullptr);
        if (device && !device->encodesIntraRefresh()) {
            std::fprintf(stderr,
                         "  skipped: this driver has no VK_KHR_video_encode_intra_refresh\n");
        } else if (device) {
            // The stream's own: two seconds a sweep, every four periods.
            encode::VulkanHevcEncoder stream;
            CHECK(stream.init(device, Codec::Hevc, 1920, 1080, 60, 20000, true, tuning, error));
            CHECK(stream.intraRefreshEnabled());
            std::fprintf(stderr, "  the stream's: horizon %d pictures\n",
                         stream.intraRefreshFrames());
            // 480 + 120 where the driver takes a 120-picture sweep; less where
            // it caps the sweep, never more.
            CHECK(stream.intraRefreshFrames() > 0 && stream.intraRefreshFrames() <= 600);
            stream.stop();

            // The proof's: four pictures back to back, a loss in the second.
            encode::VulkanHevcEncoder::Witness witness;
            witness.sweepPictures = 4;
            encode::VulkanHevcEncoder encoder;
            const bool ok = encoder.init(device, Codec::Hevc, 1280, 720, 60, 20000, true, tuning,
                                         error, witness);
            if (!ok) std::fprintf(stderr, "  init: %s\n", error.c_str());
            CHECK(ok);
            if (ok) {
                CHECK(encoder.intraRefreshEnabled());
                CHECK_EQ(encoder.intraRefreshFrames(), 4);
                std::vector<uint8_t> grey(1280 * 720 * 3 / 2, 128);
                int keyframes = 0;
                for (uint32_t n = 0; n < 14; ++n) {
                    for (size_t i = 0; i < 1280 * 720; ++i)
                        grey[i] = static_cast<uint8_t>(16 + (i / 1280 + n * 9) % 200);
                    CHECK(encoder.upload(grey.data(), error));
                    // Frames 5 and 6 lost, reported before 7: 7 predicts from
                    // an older picture kept, and the sweep that began at 4
                    // starts over from it — a wholly dirty reference.
                    if (n == 7) CHECK(encoder.invalidateReference(5, error));
                    encode::EncoderOutput out;
                    const bool encoded = encoder.encode(false, n, out, error);
                    if (!encoded) std::fprintf(stderr, "  picture %u: %s\n", n, error.c_str());
                    CHECK(encoded);
                    if (!encoded) break;
                    keyframes += out.keyframe ? 1 : 0;
                    encoder.releaseOutput();
                }
                // The first picture only: the repair is a delta, the sweeps
                // replace every other keyframe.
                CHECK_EQ(keyframes, 1);
                CHECK(!encoder.lost());
            }
        }
    }

    SECTION("VulkanHevcProof — the product's encoder decodes back to what went in, at 60 fps and "
            "at 120, where the repair predicts from the IDR kept since picture 0");
    struct Case
    {
        int width, height, fps;
    };
    for (const Case& c : {Case{1920, 1080, 60}, Case{1280, 720, 60}, Case{1920, 1080, 120}}) {
        const encode::VulkanHevcProof proof =
            encode::proveVulkanHevc(node, c.width, c.height, c.fps, tuning);
        std::fprintf(stderr, "  %dx%d@%d: %s, %s in %lld ms — %s\n", c.width, c.height, c.fps,
                     proof.ran ? "ran" : "did not run", proof.passed ? "passed" : "FAILED",
                     static_cast<long long>(proof.tookMs), proof.summary.c_str());
        if (!id.decodesHevc) {
            // No decoder to read it back with: no proof, and no trust.
            CHECK(!proof.passed);
            continue;
        }
        CHECK(proof.ran);
        CHECK(proof.passed);
        CHECK_EQ(proof.pictures, 10); // twelve encoded, two lost on the way
        // The sweeps are part of what is proven, where the driver has them.
        if (id.decodesHevc && proof.ran)
            CHECK(contains(proof.summary, "with intra-refresh sweeps") ||
                  contains(proof.summary, "no intra refresh on this driver"));
    }

    SECTION("VulkanHevcProof — the witness: transform depth 2, what coded wrong on the 780M");
    if (id.decodesHevc) {
        encode::VulkanHevcEncoder::Witness witness;
        witness.transformDepth = 2;
        const encode::VulkanHevcProof proof =
            encode::proveVulkanHevc(node, 1920, 1080, 60, tuning, witness);
        std::fprintf(stderr, "  depth 2: %s — %s\n", proof.passed ? "passed" : "failed",
                     proof.summary.c_str());
        CHECK(proof.ran);
        // AMD's firmware codes the full depth whatever the SPS says (§8o.3):
        // there, the proof must catch it. Elsewhere it may well be right.
        if (id.vendorId == 0x1002) {
            CHECK(!proof.passed);
            CHECK(contains(proof.summary, "does not decode to what was encoded"));
        }
    }

    SECTION("VulkanHevcProof — the verdict kept in the user's cache, and asked again when not");
    if (id.decodesHevc) {
        char dir[] = "/tmp/mw-vk-proof-XXXXXX";
        if (::mkdtemp(dir)) {
            ::setenv("XDG_CACHE_HOME", dir, 1);
            ::unsetenv("MW_VK_PROOF_CACHE");
            const encode::VulkanHevcProof first =
                encode::vulkanHevcVerdict(node, 1920, 1080, 60, tuning);
            const encode::VulkanHevcProof second =
                encode::vulkanHevcVerdict(node, 1920, 1080, 60, tuning);
            CHECK(!first.cached);
            CHECK(second.cached);
            CHECK_EQ(second.passed, first.passed);
            CHECK_EQ(second.pictures, first.pictures);
            // Another size is another verdict.
            const encode::VulkanHevcProof other =
                encode::vulkanHevcVerdict(node, 1280, 720, 60, tuning);
            CHECK(!other.cached);
            // The witness is part of the key: never a pass borrowed from depth 4.
            ::setenv("MW_VK_ENCODE_DEPTH", "2", 1);
            const encode::VulkanHevcProof witnessed =
                encode::vulkanHevcVerdict(node, 1920, 1080, 60, tuning);
            ::unsetenv("MW_VK_ENCODE_DEPTH");
            CHECK(!witnessed.cached);
            if (id.vendorId == 0x1002) CHECK(!witnessed.passed);
            const std::string file = std::string(dir) + "/MoonlightWeb/vulkan-video-proofs.txt";
            struct stat st = {};
            CHECK(::stat(file.c_str(), &st) == 0);
            ::unlink(file.c_str());
            ::rmdir((std::string(dir) + "/MoonlightWeb").c_str());
            ::rmdir(dir);
            ::unsetenv("XDG_CACHE_HOME");
        }
    }
    ::unsetenv("MW_VK_PROOF_CACHE");
#endif
}
