// Android's libvulkan does not export the Vulkan 1.3 commands at every API level the port runs on.
// The renderer's are defined here, each forwarding to the device's own, so its call sites are the
// same on every platform.
#ifdef __ANDROID__

#include "gfx/context.hpp"

namespace {

PFN_vkCmdBeginRendering       g_begin_rendering = nullptr;
PFN_vkCmdEndRendering         g_end_rendering = nullptr;
PFN_vkCmdPipelineBarrier2     g_pipeline_barrier2 = nullptr;
PFN_vkQueueSubmit2            g_queue_submit2 = nullptr;
PFN_vkCmdSetCullMode          g_set_cull_mode = nullptr;
PFN_vkCmdSetFrontFace         g_set_front_face = nullptr;
PFN_vkCmdSetPrimitiveTopology g_set_primitive_topology = nullptr;
PFN_vkCmdSetDepthTestEnable   g_set_depth_test_enable = nullptr;
PFN_vkCmdSetDepthWriteEnable  g_set_depth_write_enable = nullptr;
PFN_vkCmdSetDepthCompareOp    g_set_depth_compare_op = nullptr;
PFN_vkCmdSetStencilTestEnable g_set_stencil_test_enable = nullptr;
PFN_vkCmdSetStencilOp         g_set_stencil_op = nullptr;

template <class Command>
void Load(VkDevice device, Command &command, const char *name) {
    command = reinterpret_cast<Command>(vkGetDeviceProcAddr(device, name));
    if (command == nullptr) {
        gfx::detail::Fatal("the device lacks %s", name);
    }
}

} // namespace

namespace gfx::detail {

void LoadCoreDeviceCommands(VkDevice device) {
    Load(device, g_begin_rendering, "vkCmdBeginRendering");
    Load(device, g_end_rendering, "vkCmdEndRendering");
    Load(device, g_pipeline_barrier2, "vkCmdPipelineBarrier2");
    Load(device, g_queue_submit2, "vkQueueSubmit2");
    Load(device, g_set_cull_mode, "vkCmdSetCullMode");
    Load(device, g_set_front_face, "vkCmdSetFrontFace");
    Load(device, g_set_primitive_topology, "vkCmdSetPrimitiveTopology");
    Load(device, g_set_depth_test_enable, "vkCmdSetDepthTestEnable");
    Load(device, g_set_depth_write_enable, "vkCmdSetDepthWriteEnable");
    Load(device, g_set_depth_compare_op, "vkCmdSetDepthCompareOp");
    Load(device, g_set_stencil_test_enable, "vkCmdSetStencilTestEnable");
    Load(device, g_set_stencil_op, "vkCmdSetStencilOp");
}

} // namespace gfx::detail

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo *info) {
    g_begin_rendering(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cmd) { g_end_rendering(cmd); }

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo *dependency) {
    g_pipeline_barrier2(cmd, dependency);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits,
                                              VkFence fence) {
    return g_queue_submit2(queue, count, submits, fence);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer cmd, VkCullModeFlags mode) { g_set_cull_mode(cmd, mode); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer cmd, VkFrontFace face) { g_set_front_face(cmd, face); }

VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer cmd, VkPrimitiveTopology topology) {
    g_set_primitive_topology(cmd, topology);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer cmd, VkBool32 enable) {
    g_set_depth_test_enable(cmd, enable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer cmd, VkBool32 enable) {
    g_set_depth_write_enable(cmd, enable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer cmd, VkCompareOp op) {
    g_set_depth_compare_op(cmd, op);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer cmd, VkBool32 enable) {
    g_set_stencil_test_enable(cmd, enable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer cmd, VkStencilFaceFlags faces, VkStencilOp fail,
                                             VkStencilOp pass, VkStencilOp depth_fail, VkCompareOp compare) {
    g_set_stencil_op(cmd, faces, fail, pass, depth_fail, compare);
}

#endif
