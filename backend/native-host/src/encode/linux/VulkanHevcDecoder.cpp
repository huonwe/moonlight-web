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

#include "VulkanHevcDecoder.h"

#include "../../platform/linux/vulkan/VulkanDevice.h"
#include "../HevcSliceParser.h"

#include <algorithm>
#include <cstring>

namespace mw::native::encode {

using vulkan::resultText;

namespace {

uint32_t alignUp(uint32_t v, uint32_t a)
{
    return a > 1 ? (v + a - 1) / a * a : v;
}

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a)
{
    return a > 1 ? (v + a - 1) / a * a : v;
}

/// HEVC Main, 4:2:0, 8 bits, decoded.
struct Profile
{
    VkVideoDecodeH265ProfileInfoKHR h265 = {};
    VkVideoProfileInfoKHR info = {};
    VkVideoProfileListInfoKHR list = {};

    Profile()
    {
        h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
        h265.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
        info.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
        info.pNext = &h265;
        info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
        info.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
        info.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
        info.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
        list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
        list.profileCount = 1;
        list.pProfiles = &info;
    }
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
};

/// general_level_idc (30 × the level) as the StdVideo enum.
StdVideoH265LevelIdc stdLevel(uint32_t levelIdc)
{
    static const uint32_t levels[] = {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
    for (uint32_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i)
        if (levelIdc <= levels[i]) return static_cast<StdVideoH265LevelIdc>(i);
    return STD_VIDEO_H265_LEVEL_IDC_6_2;
}

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                   uint32_t layers = 1)
{
    VkImageMemoryBarrier2 b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    return b;
}

} // namespace

struct VulkanHevcDecoder::Impl
{
    std::shared_ptr<vulkan::VulkanDevice> device;
    const vulkan::DeviceFunctions* fn = nullptr;
    VkDevice dev = VK_NULL_HANDLE;
    Profile profile;

    HevcSpsFields sps;
    HevcPpsFields pps;

    VkVideoDecodeH265CapabilitiesKHR h265Caps = {};
    VkVideoDecodeCapabilitiesKHR decodeCaps = {};
    VkVideoCapabilitiesKHR caps = {};
    uint32_t allocWidth = 0;
    uint32_t allocHeight = 0;
    uint32_t slots = 0;

    VkImage output = VK_NULL_HANDLE;
    VkDeviceMemory outputMemory = VK_NULL_HANDLE;
    VkImageView outputView = VK_NULL_HANDLE;
    VkImage dpb = VK_NULL_HANDLE;
    VkDeviceMemory dpbMemory = VK_NULL_HANDLE;
    VkImageView dpbView = VK_NULL_HANDLE;
    bool dpbTouched = false;
    bool outputTouched = false;

    VkBuffer bitstream = VK_NULL_HANDLE;
    VkDeviceMemory bitstreamMemory = VK_NULL_HANDLE;
    uint8_t* bitstreamCpu = nullptr;
    VkDeviceSize bitstreamSize = 0;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    uint8_t* readbackCpu = nullptr;

    VkVideoSessionKHR session = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> sessionMemory;
    VkVideoSessionParametersKHR parameters = VK_NULL_HANDLE;
    VkCommandPool decodePool = VK_NULL_HANDLE;
    VkCommandBuffer decodeCmd = VK_NULL_HANDLE;
    VkCommandPool copyPool = VK_NULL_HANDLE;
    VkCommandBuffer copyCmd = VK_NULL_HANDLE;
    bool first = true;

    /// The pictures the decoder holds: their POC and their DPB slot.
    struct Held
    {
        int32_t poc = 0;
        int slot = -1;
    };
    std::vector<Held> held;
    /// POC of the previous picture of temporal layer 0 (8.3.1).
    int32_t prevTid0Poc = 0;

    VkResult allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags want,
                      VkMemoryPropertyFlags fallback, VkDeviceMemory& out)
    {
        uint32_t type = device->memoryType(req.memoryTypeBits, want);
        if (type == UINT32_MAX) type = device->memoryType(req.memoryTypeBits, fallback);
        if (type == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        return fn->vkAllocateMemory(dev, &mai, nullptr, &out);
    }
};

VulkanHevcDecoder::VulkanHevcDecoder()
    : d(std::make_unique<Impl>())
{}

VulkanHevcDecoder::~VulkanHevcDecoder()
{
    stop();
}

bool VulkanHevcDecoder::init(const std::shared_ptr<vulkan::VulkanDevice>& device,
                             const std::vector<uint8_t>& parameterSets, std::string& error)
{
    stop();
    if (!device || !device->decodeQueue()) {
        error = "no Vulkan device with a decode queue";
        return false;
    }
    d->device = device;
    d->fn = &device->fn();
    d->dev = device->device();

    // ── The stream, as its parameter sets say ──
    bool haveSps = false, havePps = false;
    std::string unread;
    for (const HevcNalUnit& u : hevcNalUnits(parameterSets.data(), parameterSets.size())) {
        if (u.type() == 33) {
            unread += parseHevcSps(u.data, u.size, d->sps);
            haveSps = true;
        } else if (u.type() == 34) {
            unread += parseHevcPps(u.data, u.size, d->pps);
            havePps = true;
        }
    }
    if (!haveSps || !havePps || !unread.empty()) {
        error = "the parameter sets do not read: " +
                (unread.empty() ? std::string("no SPS or no PPS") : unread);
        stop();
        return false;
    }
    const HevcSpsFields& s = d->sps;
    if (s.chromaFormatIdc != 1 || s.bitDepthLuma != 8 || s.bitDepthChroma != 8) {
        error = "only 8-bit 4:2:0 is decoded here";
        stop();
        return false;
    }
    if (s.longTermReferences) {
        error = "long-term pictures are not followed here";
        stop();
        return false;
    }
    if (d->pps.tiles) {
        error = "tiles are not followed here";
        stop();
        return false;
    }
    const uint32_t cropW = 2 * (s.cropLeft + s.cropRight), cropH = 2 * (s.cropTop + s.cropBottom);
    if (cropW >= s.width || cropH >= s.height) {
        error = "a conformance window larger than the picture";
        stop();
        return false;
    }
    m_Width = static_cast<int>(s.width - cropW);
    m_Height = static_cast<int>(s.height - cropH);

    // ── What the driver takes ──
    d->h265Caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;
    d->decodeCaps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
    d->decodeCaps.pNext = &d->h265Caps;
    d->caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    d->caps.pNext = &d->decodeCaps;
    const VkResult r = device->videoCapabilities(d->profile.info, d->caps);
    if (r != VK_SUCCESS) {
        error =
            device->name() + " decodes no HEVC Main through Vulkan Video (" + resultText(r) + ")";
        stop();
        return false;
    }
    // A separate output picture: what VCN before 5 does (RADV). A decoder
    // that writes its output only into its DPB is not followed yet.
    if (!(d->decodeCaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR)) {
        error = "the decoder writes its output only into its reference pictures: not followed";
        stop();
        return false;
    }
    if (s.width < d->caps.minCodedExtent.width || s.height < d->caps.minCodedExtent.height ||
        s.width > d->caps.maxCodedExtent.width || s.height > d->caps.maxCodedExtent.height) {
        error = "the Vulkan decoder takes no " + std::to_string(s.width) + "x" +
                std::to_string(s.height);
        stop();
        return false;
    }
    d->allocWidth = alignUp(s.width, std::max(d->caps.pictureAccessGranularity.width, 1u));
    d->allocHeight = alignUp(s.height, std::max(d->caps.pictureAccessGranularity.height, 1u));
    // Every picture the SPS lets the stream keep, and the one being decoded.
    d->slots = s.maxDecPicBufferingMinus1 + 2;
    if (d->slots > d->caps.maxDpbSlots) {
        error = "the Vulkan decoder holds " + std::to_string(d->caps.maxDpbSlots) +
                " pictures, the stream keeps up to " + std::to_string(d->slots - 1);
        stop();
        return false;
    }
    if (!createResources(error) || !createSession(error)) {
        stop();
        return false;
    }
    return true;
}

bool VulkanHevcDecoder::createResources(std::string& error)
{
    const vulkan::DeviceFunctions& fn = *d->fn;
    VkDevice dev = d->dev;
    vulkan::VulkanDevice& device = *d->device;
    const uint32_t families[2] = {device.decodeFamily(), device.family()};
    const bool shared = families[0] != families[1];

    auto format = [&](VkImageUsageFlags usage) {
        VkPhysicalDeviceVideoFormatInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
        info.pNext = &d->profile.list;
        info.imageUsage = usage;
        std::vector<VkVideoFormatPropertiesKHR> formats;
        return device.videoFormats(info, formats) == VK_SUCCESS &&
               std::any_of(formats.begin(), formats.end(), [](const auto& f) {
                   return f.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM &&
                          f.imageTiling == VK_IMAGE_TILING_OPTIMAL;
               });
    };
    const VkImageUsageFlags outputUsage =
        VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!format(outputUsage) || !format(VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR)) {
        error = "the Vulkan decoder has no NV12 picture the CPU can read back";
        return false;
    }

    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &d->profile.list;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ici.extent = {d->allocWidth, d->allocHeight, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = outputUsage;
    ici.sharingMode = shared ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    ici.queueFamilyIndexCount = shared ? 2 : 0;
    ici.pQueueFamilyIndices = shared ? families : nullptr;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements req = {};
    VkResult r = fn.vkCreateImage(dev, &ici, nullptr, &d->output);
    if (r == VK_SUCCESS) {
        fn.vkGetImageMemoryRequirements(dev, d->output, &req);
        r = d->allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, d->outputMemory);
    }
    if (r == VK_SUCCESS) r = fn.vkBindImageMemory(dev, d->output, d->outputMemory, 0);
    ici.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    ici.arrayLayers = d->slots;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.queueFamilyIndexCount = 0;
    ici.pQueueFamilyIndices = nullptr;
    if (r == VK_SUCCESS) r = fn.vkCreateImage(dev, &ici, nullptr, &d->dpb);
    if (r == VK_SUCCESS) {
        fn.vkGetImageMemoryRequirements(dev, d->dpb, &req);
        r = d->allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, d->dpbMemory);
    }
    if (r == VK_SUCCESS) r = fn.vkBindImageMemory(dev, d->dpb, d->dpbMemory, 0);
    auto view = [&](VkImage image, VkImageViewType type, VkImageUsageFlags usage, uint32_t layers,
                    VkImageView& out) {
        VkImageViewUsageCreateInfo vu = {};
        vu.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
        vu.usage = usage;
        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.pNext = &vu;
        vci.image = image;
        vci.viewType = type;
        vci.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        return fn.vkCreateImageView(dev, &vci, nullptr, &out);
    };
    if (r == VK_SUCCESS)
        r = view(d->output, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR, 1,
                 d->outputView);
    if (r == VK_SUCCESS)
        r = view(d->dpb, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR,
                 d->slots, d->dpbView);
    if (r != VK_SUCCESS) {
        error = "the Vulkan decoder's pictures: " + resultText(r);
        return false;
    }

    // ── The bitstream in, the picture out: both in memory the CPU maps ──
    const VkMemoryPropertyFlags visible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkDeviceSize picture = static_cast<VkDeviceSize>(d->sps.width) * d->sps.height * 3 / 2;
    d->bitstreamSize = alignUp(picture + 65536,
                               std::max<VkDeviceSize>(d->caps.minBitstreamBufferSizeAlignment, 1));
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &d->profile.list;
    bci.size = d->bitstreamSize;
    bci.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = fn.vkCreateBuffer(dev, &bci, nullptr, &d->bitstream);
    if (r == VK_SUCCESS) {
        fn.vkGetBufferMemoryRequirements(dev, d->bitstream, &req);
        r = d->allocate(req, visible, visible, d->bitstreamMemory);
    }
    if (r == VK_SUCCESS) r = fn.vkBindBufferMemory(dev, d->bitstream, d->bitstreamMemory, 0);
    if (r == VK_SUCCESS)
        r = fn.vkMapMemory(dev, d->bitstreamMemory, 0, VK_WHOLE_SIZE, 0,
                           reinterpret_cast<void**>(&d->bitstreamCpu));
    bci.pNext = nullptr;
    bci.size = picture;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (r == VK_SUCCESS) r = fn.vkCreateBuffer(dev, &bci, nullptr, &d->readback);
    if (r == VK_SUCCESS) {
        fn.vkGetBufferMemoryRequirements(dev, d->readback, &req);
        r = d->allocate(req, visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, visible,
                        d->readbackMemory);
    }
    if (r == VK_SUCCESS) r = fn.vkBindBufferMemory(dev, d->readback, d->readbackMemory, 0);
    if (r == VK_SUCCESS)
        r = fn.vkMapMemory(dev, d->readbackMemory, 0, VK_WHOLE_SIZE, 0,
                           reinterpret_cast<void**>(&d->readbackCpu));
    if (r != VK_SUCCESS) {
        error = "the Vulkan decoder's buffers: " + resultText(r);
        return false;
    }

    for (int q = 0; q < 2; ++q) {
        VkCommandPoolCreateInfo pci = {};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = q == 0 ? device.decodeFamily() : device.family();
        VkCommandPool& pool = q == 0 ? d->decodePool : d->copyPool;
        r = fn.vkCreateCommandPool(dev, &pci, nullptr, &pool);
        if (r != VK_SUCCESS) break;
        VkCommandBufferAllocateInfo cai = {};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        r = fn.vkAllocateCommandBuffers(dev, &cai, q == 0 ? &d->decodeCmd : &d->copyCmd);
        if (r != VK_SUCCESS) break;
    }
    if (r != VK_SUCCESS) {
        error = "the Vulkan decoder's commands: " + resultText(r);
        return false;
    }
    return true;
}

bool VulkanHevcDecoder::createSession(std::string& error)
{
    const vulkan::DeviceFunctions& fn = *d->fn;
    VkDevice dev = d->dev;
    const HevcSpsFields& s = d->sps;
    const HevcPpsFields& p = d->pps;
    VkVideoSessionCreateInfoKHR sci = {};
    sci.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    sci.queueFamilyIndex = d->device->decodeFamily();
    sci.pVideoProfile = &d->profile.info;
    sci.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    sci.maxCodedExtent = {s.width, s.height};
    sci.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    sci.maxDpbSlots = d->slots;
    sci.maxActiveReferencePictures = std::min(d->caps.maxActiveReferencePictures, d->slots - 1);
    sci.pStdHeaderVersion = &d->caps.stdHeaderVersion;
    VkResult r = fn.vkCreateVideoSessionKHR(dev, &sci, nullptr, &d->session);
    if (r != VK_SUCCESS) {
        d->session = VK_NULL_HANDLE;
        error = "the Vulkan decode session: " + resultText(r);
        return false;
    }
    uint32_t n = 0;
    fn.vkGetVideoSessionMemoryRequirementsKHR(dev, d->session, &n, nullptr);
    std::vector<VkVideoSessionMemoryRequirementsKHR> reqs(n);
    for (auto& q : reqs) {
        q = {};
        q.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    }
    fn.vkGetVideoSessionMemoryRequirementsKHR(dev, d->session, &n, reqs.data());
    std::vector<VkBindVideoSessionMemoryInfoKHR> binds(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        r = d->allocate(reqs[i].memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, memory);
        if (r != VK_SUCCESS) {
            error = "the Vulkan decode session's memory: " + resultText(r);
            return false;
        }
        d->sessionMemory.push_back(memory);
        binds[i] = {};
        binds[i].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        binds[i].memoryBindIndex = reqs[i].memoryBindIndex;
        binds[i].memory = memory;
        binds[i].memorySize = reqs[i].memoryRequirements.size;
    }
    r = fn.vkBindVideoSessionMemoryKHR(dev, d->session, n, binds.data());
    if (r != VK_SUCCESS) {
        error = "vkBindVideoSessionMemoryKHR (decode): " + resultText(r);
        return false;
    }

    // ── The parameter sets, as the stream wrote them ──
    StdVideoH265ProfileTierLevel ptl = {};
    ptl.flags.general_tier_flag = s.highTier;
    ptl.flags.general_progressive_source_flag = s.progressiveSource;
    ptl.flags.general_interlaced_source_flag = s.interlacedSource;
    ptl.flags.general_non_packed_constraint_flag = s.nonPackedConstraint;
    ptl.flags.general_frame_only_constraint_flag = s.frameOnlyConstraint;
    ptl.general_profile_idc = static_cast<StdVideoH265ProfileIdc>(s.profileIdc);
    ptl.general_level_idc = stdLevel(s.levelIdc);
    StdVideoH265DecPicBufMgr dpbMgr = {};
    for (uint32_t i = 0; i <= s.maxSubLayersMinus1 && i < STD_VIDEO_H265_SUBLAYERS_LIST_SIZE; ++i) {
        dpbMgr.max_dec_pic_buffering_minus1[i] = static_cast<uint8_t>(s.maxDecPicBufferingMinus1);
        dpbMgr.max_num_reorder_pics[i] = static_cast<uint8_t>(s.maxNumReorderPics);
        dpbMgr.max_latency_increase_plus1[i] = s.maxLatencyIncreasePlus1;
    }
    // The VPS says nothing a decoder uses here: the SPS's own values.
    StdVideoH265VideoParameterSet vps = {};
    vps.flags.vps_temporal_id_nesting_flag = s.temporalIdNesting;
    vps.flags.vps_sub_layer_ordering_info_present_flag = 1;
    vps.vps_video_parameter_set_id = static_cast<uint8_t>(s.vpsId);
    vps.vps_max_sub_layers_minus1 = static_cast<uint8_t>(s.maxSubLayersMinus1);
    vps.pDecPicBufMgr = &dpbMgr;
    vps.pProfileTierLevel = &ptl;
    StdVideoH265SequenceParameterSet sps = {};
    sps.flags.sps_temporal_id_nesting_flag = s.temporalIdNesting;
    sps.flags.conformance_window_flag = s.cropLeft || s.cropRight || s.cropTop || s.cropBottom;
    sps.flags.sps_sub_layer_ordering_info_present_flag = 1;
    sps.flags.scaling_list_enabled_flag = s.scalingList;
    sps.flags.amp_enabled_flag = s.asymmetricMotionPartitions;
    sps.flags.sample_adaptive_offset_enabled_flag = s.sampleAdaptiveOffset;
    sps.flags.pcm_enabled_flag = s.pcm;
    sps.flags.pcm_loop_filter_disabled_flag = s.pcmLoopFilterDisabled;
    sps.flags.sps_temporal_mvp_enabled_flag = s.temporalMvp;
    sps.flags.strong_intra_smoothing_enabled_flag = s.strongIntraSmoothing;
    sps.chroma_format_idc = STD_VIDEO_H265_CHROMA_FORMAT_IDC_420;
    sps.pic_width_in_luma_samples = s.width;
    sps.pic_height_in_luma_samples = s.height;
    sps.sps_video_parameter_set_id = static_cast<uint8_t>(s.vpsId);
    sps.sps_max_sub_layers_minus1 = static_cast<uint8_t>(s.maxSubLayersMinus1);
    sps.sps_seq_parameter_set_id = static_cast<uint8_t>(s.spsId);
    sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<uint8_t>(s.log2MaxPocLsb - 4);
    sps.log2_min_luma_coding_block_size_minus3 = static_cast<uint8_t>(s.log2MinCodingBlock - 3);
    sps.log2_diff_max_min_luma_coding_block_size =
        static_cast<uint8_t>(s.log2CodingTreeBlock - s.log2MinCodingBlock);
    sps.log2_min_luma_transform_block_size_minus2 =
        static_cast<uint8_t>(s.log2MinTransformBlock - 2);
    sps.log2_diff_max_min_luma_transform_block_size =
        static_cast<uint8_t>(s.log2MaxTransformBlock - s.log2MinTransformBlock);
    // The depth the stream's SPS says — the whole point of the proof: a
    // decoder told this depth reads the split flags where the SPS put them.
    sps.max_transform_hierarchy_depth_inter =
        static_cast<uint8_t>(s.maxTransformHierarchyDepthInter);
    sps.max_transform_hierarchy_depth_intra =
        static_cast<uint8_t>(s.maxTransformHierarchyDepthIntra);
    if (s.pcm) {
        sps.pcm_sample_bit_depth_luma_minus1 = static_cast<uint8_t>(s.pcmBitDepthLuma - 1);
        sps.pcm_sample_bit_depth_chroma_minus1 = static_cast<uint8_t>(s.pcmBitDepthChroma - 1);
        sps.log2_min_pcm_luma_coding_block_size_minus3 =
            static_cast<uint8_t>(s.log2MinPcmCodingBlock - 3);
        sps.log2_diff_max_min_pcm_luma_coding_block_size =
            static_cast<uint8_t>(s.log2MaxPcmCodingBlock - s.log2MinPcmCodingBlock);
    }
    sps.conf_win_left_offset = s.cropLeft;
    sps.conf_win_right_offset = s.cropRight;
    sps.conf_win_top_offset = s.cropTop;
    sps.conf_win_bottom_offset = s.cropBottom;
    sps.pProfileTierLevel = &ptl;
    sps.pDecPicBufMgr = &dpbMgr;
    StdVideoH265PictureParameterSet pps = {};
    pps.flags.dependent_slice_segments_enabled_flag = p.dependentSliceSegments;
    pps.flags.output_flag_present_flag = p.outputFlagPresent;
    pps.flags.sign_data_hiding_enabled_flag = p.signDataHiding;
    pps.flags.cabac_init_present_flag = p.cabacInitPresent;
    pps.flags.constrained_intra_pred_flag = p.constrainedIntraPred;
    pps.flags.transform_skip_enabled_flag = p.transformSkip;
    pps.flags.cu_qp_delta_enabled_flag = p.cuQpDelta;
    pps.flags.pps_slice_chroma_qp_offsets_present_flag = p.sliceChromaQpOffsets;
    pps.flags.weighted_pred_flag = p.weightedPred;
    pps.flags.weighted_bipred_flag = p.weightedBipred;
    pps.flags.transquant_bypass_enabled_flag = p.transquantBypass;
    pps.flags.entropy_coding_sync_enabled_flag = p.entropyCodingSync;
    pps.flags.pps_loop_filter_across_slices_enabled_flag = p.loopFilterAcrossSlices;
    pps.flags.deblocking_filter_control_present_flag = p.deblockingControlPresent;
    pps.flags.deblocking_filter_override_enabled_flag = p.deblockingOverride;
    pps.flags.pps_deblocking_filter_disabled_flag = p.deblockingDisabled;
    pps.flags.lists_modification_present_flag = p.listsModification;
    pps.flags.slice_segment_header_extension_present_flag = p.sliceHeaderExtension;
    pps.pps_pic_parameter_set_id = static_cast<uint8_t>(p.ppsId);
    pps.pps_seq_parameter_set_id = static_cast<uint8_t>(p.spsId);
    pps.sps_video_parameter_set_id = static_cast<uint8_t>(s.vpsId);
    pps.num_extra_slice_header_bits = static_cast<uint8_t>(p.extraSliceHeaderBits);
    pps.num_ref_idx_l0_default_active_minus1 = static_cast<uint8_t>(p.defaultActiveL0 - 1);
    pps.num_ref_idx_l1_default_active_minus1 = static_cast<uint8_t>(p.defaultActiveL1 - 1);
    pps.init_qp_minus26 = static_cast<int8_t>(p.initQp - 26);
    pps.diff_cu_qp_delta_depth = static_cast<uint8_t>(p.diffCuQpDeltaDepth);
    pps.pps_cb_qp_offset = static_cast<int8_t>(p.cbQpOffset);
    pps.pps_cr_qp_offset = static_cast<int8_t>(p.crQpOffset);
    pps.pps_beta_offset_div2 = static_cast<int8_t>(p.betaOffsetDiv2);
    pps.pps_tc_offset_div2 = static_cast<int8_t>(p.tcOffsetDiv2);
    pps.log2_parallel_merge_level_minus2 = static_cast<uint8_t>(p.log2ParallelMergeLevel - 2);

    VkVideoDecodeH265SessionParametersAddInfoKHR add = {};
    add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR;
    add.stdVPSCount = 1;
    add.pStdVPSs = &vps;
    add.stdSPSCount = 1;
    add.pStdSPSs = &sps;
    add.stdPPSCount = 1;
    add.pStdPPSs = &pps;
    VkVideoDecodeH265SessionParametersCreateInfoKHR h265p = {};
    h265p.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR;
    h265p.maxStdVPSCount = 1;
    h265p.maxStdSPSCount = 1;
    h265p.maxStdPPSCount = 1;
    h265p.pParametersAddInfo = &add;
    VkVideoSessionParametersCreateInfoKHR pci = {};
    pci.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
    pci.pNext = &h265p;
    pci.videoSession = d->session;
    r = fn.vkCreateVideoSessionParametersKHR(dev, &pci, nullptr, &d->parameters);
    if (r != VK_SUCCESS) {
        d->parameters = VK_NULL_HANDLE;
        error = "the Vulkan decoder refuses the stream's parameter sets: " + resultText(r);
        return false;
    }
    return true;
}

bool VulkanHevcDecoder::decode(const uint8_t* data, size_t size, std::vector<uint8_t>& nv12,
                               std::string& error)
{
    if (!d->session) {
        error = "the decoder is not initialized";
        return false;
    }
    const HevcSpsFields& s = d->sps;

    // ── The picture's slices, and what the first one says ──
    std::vector<uint32_t> sliceOffsets;
    HevcSliceFields f;
    bool haveSlice = false;
    size_t firstSlice = size;
    for (const HevcNalUnit& u : hevcNalUnits(data, size)) {
        const uint8_t type = u.type();
        if (type > 21 || (type > 9 && type < 16)) continue; // parameter sets, SEI, AUD
        // The start code is the three bytes in front of the unit.
        const size_t at = static_cast<size_t>(u.data - data) - 3;
        if (!haveSlice) {
            const std::string bad = parseHevcSliceHeader(u.data, u.size, s, d->pps, f);
            if (!bad.empty()) {
                error = "the slice header does not read: " + bad;
                return false;
            }
            haveSlice = true;
            firstSlice = at;
        }
        sliceOffsets.push_back(static_cast<uint32_t>(at - firstSlice));
    }
    if (!haveSlice) {
        error = "no slice in the access unit";
        return false;
    }
    const size_t bytes = size - firstSlice;
    const VkDeviceSize range =
        alignUp(static_cast<VkDeviceSize>(bytes),
                std::max<VkDeviceSize>(d->caps.minBitstreamBufferSizeAlignment, 1));
    if (range > d->bitstreamSize) {
        error = "an access unit of " + std::to_string(bytes) + " bytes";
        return false;
    }
    std::memcpy(d->bitstreamCpu, data + firstSlice, bytes);
    std::memset(d->bitstreamCpu + bytes, 0, static_cast<size_t>(range - bytes));

    // ── Its place (8.3.1) and its references (8.3.2) ──
    const bool irap = f.nalType >= 16 && f.nalType <= 23;
    const bool idr = f.nalType == 19 || f.nalType == 20;
    const bool reference = irap || (f.nalType <= 14 && (f.nalType & 1));
    int32_t poc = 0;
    if (idr) {
        d->held.clear();
    } else {
        const int32_t maxLsb = 1 << s.log2MaxPocLsb;
        const int32_t prevLsb = d->prevTid0Poc & (maxLsb - 1);
        const int32_t prevMsb = d->prevTid0Poc - prevLsb;
        const int32_t lsb = static_cast<int32_t>(f.pocLsb);
        int32_t msb = prevMsb;
        if (lsb < prevLsb && prevLsb - lsb >= maxLsb / 2)
            msb = prevMsb + maxLsb;
        else if (lsb > prevLsb && lsb - prevLsb > maxLsb / 2)
            msb = prevMsb - maxLsb;
        poc = msb + lsb;
    }
    std::vector<Impl::Held> kept;
    uint8_t before[STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE];
    uint8_t after[STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE];
    uint8_t longTerm[STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE];
    std::memset(before, 0xFF, sizeof(before));
    std::memset(after, 0xFF, sizeof(after));
    std::memset(longTerm, 0xFF, sizeof(longTerm));
    size_t nBefore = 0, nAfter = 0;
    for (const HevcSliceFields::Reference& ref : f.shortTerm) {
        const int32_t want = poc + ref.deltaPoc;
        const auto it = std::find_if(d->held.begin(), d->held.end(),
                                     [&](const Impl::Held& h) { return h.poc == want; });
        if (it == d->held.end()) {
            // A missing reference is a loss; the proof decodes what the
            // receiver would have had, and a hole in it is an error.
            error = "picture " + std::to_string(poc) + " refers to picture " +
                    std::to_string(want) + ", which the decoder does not hold";
            return false;
        }
        kept.push_back(*it);
        if (!ref.used) continue;
        uint8_t* list = ref.deltaPoc < 0 ? before : after;
        size_t& n = ref.deltaPoc < 0 ? nBefore : nAfter;
        if (n >= STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE) {
            error = "more used references than a decoder lists";
            return false;
        }
        list[n++] = static_cast<uint8_t>(it->slot);
    }
    d->held = kept; // what the set leaves out is gone for good
    int setup = -1;
    for (int slot = 0; slot < static_cast<int>(d->slots) && setup < 0; ++slot)
        if (std::none_of(kept.begin(), kept.end(),
                         [&](const Impl::Held& h) { return h.slot == slot; }))
            setup = slot;
    if (setup < 0) {
        error = "no free picture in the decoder";
        return false;
    }

    // ── Record the decode ──
    const vulkan::DeviceFunctions& fn = *d->fn;
    VkCommandBuffer cmd = d->decodeCmd;
    fn.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn.vkBeginCommandBuffer(cmd, &cbi);
    constexpr VkPipelineStageFlags2 kDecode = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
    const VkImageMemoryBarrier2 barriers[2] = {
        // Rewritten whole: the last copy's contents are not wanted. From the
        // decode stage, the one this submission's wait holds back.
        imageBarrier(d->output,
                     d->outputTouched ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                      : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR, kDecode, VK_ACCESS_2_NONE, kDecode,
                     VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR),
        imageBarrier(
            d->dpb,
            d->dpbTouched ? VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, kDecode,
            d->dpbTouched ? VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR : VK_ACCESS_2_NONE, kDecode,
            VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
            d->slots)};
    VkDependencyInfo dep = {};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = barriers;
    fn.vkCmdPipelineBarrier2(cmd, &dep);

    std::vector<VkVideoPictureResourceInfoKHR> resources(d->slots);
    for (uint32_t i = 0; i < d->slots; ++i) {
        resources[i] = {};
        resources[i].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        resources[i].codedExtent = {s.width, s.height};
        resources[i].baseArrayLayer = i;
        resources[i].imageViewBinding = d->dpbView;
    }
    std::vector<StdVideoDecodeH265ReferenceInfo> refStd(kept.size());
    std::vector<VkVideoDecodeH265DpbSlotInfoKHR> refDpb(kept.size());
    std::vector<VkVideoReferenceSlotInfoKHR> beginSlots;
    for (size_t k = 0; k < kept.size(); ++k) {
        refStd[k] = {};
        refStd[k].PicOrderCntVal = kept[k].poc;
        refDpb[k] = {};
        refDpb[k].sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
        refDpb[k].pStdReferenceInfo = &refStd[k];
        VkVideoReferenceSlotInfoKHR slot = {};
        slot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        slot.pNext = &refDpb[k];
        slot.slotIndex = kept[k].slot;
        slot.pPictureResource = &resources[static_cast<size_t>(kept[k].slot)];
        beginSlots.push_back(slot);
    }
    {
        VkVideoReferenceSlotInfoKHR slot = {};
        slot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        slot.slotIndex = -1;
        slot.pPictureResource = &resources[static_cast<size_t>(setup)];
        beginSlots.push_back(slot);
    }
    VkVideoBeginCodingInfoKHR begin = {};
    begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    begin.videoSession = d->session;
    begin.videoSessionParameters = d->parameters;
    begin.referenceSlotCount = static_cast<uint32_t>(beginSlots.size());
    begin.pReferenceSlots = beginSlots.data();
    fn.vkCmdBeginVideoCodingKHR(cmd, &begin);
    if (d->first) {
        VkVideoCodingControlInfoKHR control = {};
        control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
        control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
        fn.vkCmdControlVideoCodingKHR(cmd, &control);
    }

    StdVideoDecodeH265PictureInfo picture = {};
    picture.flags.IrapPicFlag = irap;
    picture.flags.IdrPicFlag = idr;
    picture.flags.IsReference = reference;
    picture.sps_video_parameter_set_id = static_cast<uint8_t>(s.vpsId);
    picture.pps_seq_parameter_set_id = static_cast<uint8_t>(s.spsId);
    picture.pps_pic_parameter_set_id = static_cast<uint8_t>(d->pps.ppsId);
    picture.PicOrderCntVal = poc;
    picture.NumBitsForSTRefPicSetInSlice = static_cast<uint16_t>(f.shortTermSetBits);
    std::memcpy(picture.RefPicSetStCurrBefore, before, sizeof(before));
    std::memcpy(picture.RefPicSetStCurrAfter, after, sizeof(after));
    std::memcpy(picture.RefPicSetLtCurr, longTerm, sizeof(longTerm));
    VkVideoDecodeH265PictureInfoKHR h265Picture = {};
    h265Picture.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR;
    h265Picture.pStdPictureInfo = &picture;
    h265Picture.sliceSegmentCount = static_cast<uint32_t>(sliceOffsets.size());
    h265Picture.pSliceSegmentOffsets = sliceOffsets.data();

    StdVideoDecodeH265ReferenceInfo setupStd = {};
    setupStd.PicOrderCntVal = poc;
    VkVideoDecodeH265DpbSlotInfoKHR setupDpb = {};
    setupDpb.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
    setupDpb.pStdReferenceInfo = &setupStd;
    VkVideoReferenceSlotInfoKHR setupSlot = {};
    setupSlot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    setupSlot.pNext = &setupDpb;
    setupSlot.slotIndex = setup;
    setupSlot.pPictureResource = &resources[static_cast<size_t>(setup)];

    VkVideoDecodeInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
    info.pNext = &h265Picture;
    info.srcBuffer = d->bitstream;
    info.srcBufferOffset = 0;
    info.srcBufferRange = range;
    info.dstPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    info.dstPictureResource.codedExtent = {s.width, s.height};
    info.dstPictureResource.imageViewBinding = d->outputView;
    info.pSetupReferenceSlot = reference ? &setupSlot : nullptr;
    // Every picture kept, not only the ones this picture uses — as ffmpeg's
    // Vulkan decoder lists them. RADV on the 780M reads a picture left out
    // here wrong when a later one predicts from it: the IDR, kept unused
    // under pictures 2 and 3, then the proof's repair's reference from 73 fps
    // on (13.8 dB, where ffmpeg reads the same stream at 48; 09/10/2026).
    info.referenceSlotCount = static_cast<uint32_t>(kept.size());
    info.pReferenceSlots = kept.empty() ? nullptr : beginSlots.data();
    fn.vkCmdDecodeVideoKHR(cmd, &info);
    VkVideoEndCodingInfoKHR end = {};
    end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
    fn.vkCmdEndVideoCodingKHR(cmd, &end);
    fn.vkEndCommandBuffer(cmd);
    std::string why;
    VkSemaphoreSubmitInfo wait = d->device->afterLast(kDecode);
    if (!d->device->runOn(d->device->decodeQueue(), cmd, wait.semaphore ? &wait : nullptr,
                          wait.semaphore ? 1u : 0u, why)) {
        error = "the Vulkan decode failed: " + why;
        return false;
    }
    d->first = false;
    d->dpbTouched = true;
    if (reference) d->held.push_back({poc, setup});
    d->prevTid0Poc = poc;

    // ── The picture back to the CPU, on the compute queue ──
    cmd = d->copyCmd;
    fn.vkResetCommandBuffer(cmd, 0);
    fn.vkBeginCommandBuffer(cmd, &cbi);
    const VkImageMemoryBarrier2 toCopy =
        imageBarrier(d->output, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toCopy;
    fn.vkCmdPipelineBarrier2(cmd, &dep);
    VkBufferImageCopy regions[2] = {};
    regions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
    regions[0].imageExtent = {s.width, s.height, 1};
    regions[1].bufferOffset = static_cast<VkDeviceSize>(s.width) * s.height;
    regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
    regions[1].imageExtent = {s.width / 2, s.height / 2, 1};
    fn.vkCmdCopyImageToBuffer(cmd, d->output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->readback, 2,
                              regions);
    VkBufferMemoryBarrier2 toHost = {};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = d->readback;
    toHost.size = VK_WHOLE_SIZE;
    VkDependencyInfo hostDep = {};
    hostDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    hostDep.bufferMemoryBarrierCount = 1;
    hostDep.pBufferMemoryBarriers = &toHost;
    fn.vkCmdPipelineBarrier2(cmd, &hostDep);
    fn.vkEndCommandBuffer(cmd);
    wait = d->device->afterLast(VK_PIPELINE_STAGE_2_COPY_BIT);
    if (!d->device->run(cmd, &wait, 1, why)) {
        error = "the decoded picture did not come back: " + why;
        return false;
    }
    d->outputTouched = true;

    // ── Cropped to the conformance window ──
    const size_t w = static_cast<size_t>(m_Width), h = static_cast<size_t>(m_Height);
    const size_t left = 2 * s.cropLeft, top = 2 * s.cropTop;
    const size_t pitch = s.width;
    nv12.resize(w * h * 3 / 2);
    const uint8_t* luma = d->readbackCpu;
    const uint8_t* chroma = d->readbackCpu + static_cast<size_t>(s.width) * s.height;
    for (size_t row = 0; row < h; ++row)
        std::memcpy(nv12.data() + row * w, luma + (top + row) * pitch + left, w);
    for (size_t row = 0; row < h / 2; ++row)
        std::memcpy(nv12.data() + w * h + row * w, chroma + (top / 2 + row) * pitch + left, w);
    return true;
}

void VulkanHevcDecoder::stop()
{
    if (d && d->device) {
        const vulkan::DeviceFunctions& fn = *d->fn;
        VkDevice dev = d->dev;
        if (fn.vkDeviceWaitIdle) fn.vkDeviceWaitIdle(dev);
        if (d->decodePool) fn.vkDestroyCommandPool(dev, d->decodePool, nullptr);
        if (d->copyPool) fn.vkDestroyCommandPool(dev, d->copyPool, nullptr);
        if (d->parameters) fn.vkDestroyVideoSessionParametersKHR(dev, d->parameters, nullptr);
        if (d->session) fn.vkDestroyVideoSessionKHR(dev, d->session, nullptr);
        for (VkDeviceMemory m : d->sessionMemory)
            fn.vkFreeMemory(dev, m, nullptr);
        for (VkImageView v : {d->outputView, d->dpbView})
            if (v) fn.vkDestroyImageView(dev, v, nullptr);
        for (VkImage i : {d->output, d->dpb})
            if (i) fn.vkDestroyImage(dev, i, nullptr);
        for (VkBuffer b : {d->bitstream, d->readback})
            if (b) fn.vkDestroyBuffer(dev, b, nullptr);
        for (VkDeviceMemory m :
             {d->outputMemory, d->dpbMemory, d->bitstreamMemory, d->readbackMemory})
            if (m) fn.vkFreeMemory(dev, m, nullptr);
    }
    d = std::make_unique<Impl>();
    m_Width = 0;
    m_Height = 0;
}

} // namespace mw::native::encode
