/*
 * Renders one triangle with the vendor Vulkan driver and checks the pixels.
 *
 * A full-screen triangle with red, green and blue vertices is drawn into a 64x64 RGBA8 image
 * (render pass, graphics pipeline, command buffer, fence), the image is copied into a
 * host-visible buffer and a few pixels are compared with the expected interpolated colours.
 * Run it from /system/bin; binaries in /data get a linker namespace without the vendor driver.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "vk_tri_frag.h"
#include "vk_tri_vert.h"

#ifndef W
#define W 64
#define H 64
#endif
/* Mi Pad 3 panel, as LK leaves it: 1536x2048, 4 bytes per pixel in R,G,B,A order */
#define FB_W 1536
#define FB_H 2048
#define CHECK(x)                                                         \
    do {                                                                 \
        VkResult r_ = (x);                                               \
        if (r_ != VK_SUCCESS) {                                          \
            printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__);        \
            exit(1);                                                     \
        }                                                                \
    } while (0)

static uint32_t memtype(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    printf("FAIL no memory type for 0x%x\n", want);
    exit(1);
}

int main(int argc, char** argv) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
    if (!n) { printf("FAIL no physical device\n"); return 1; }

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties qf[8];
    if (qn > 8) qn = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qfi = 0;
    while (qfi < qn && !(qf[qfi].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfi++;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci};
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);

    /* colour target */
    VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1}, .mipLevels = 1,
                              .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage img;
    CHECK(vkCreateImage(dev, &imci, NULL, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memtype(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory imem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
    CHECK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo ivci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    CHECK(vkCreateImageView(dev, &ivci, NULL, &view));

    /* readback buffer */
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memtype(pd, mr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem));
    CHECK(vkBindBufferMemory(dev, buf, bmem, 0));

    /* render pass and framebuffer */
    VkAttachmentDescription att = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                   .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                   .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                .pColorAttachments = &ref};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                   .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub};
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    VkFramebufferCreateInfo fbci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                    .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H,
                                    .layers = 1};
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

    /* pipeline */
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = sizeof(tri_vert_spv), .pCode = tri_vert_spv};
    VkShaderModule vs, fs;
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &vs));
    smci.codeSize = sizeof(tri_frag_spv);
    smci.pCode = tri_frag_spv;
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &fs));
    VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = vs, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, W, H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .pViewports = &vp, .scissorCount = 1,
                                             .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                         .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                         .pViewportState = &vps, .pRasterizationState = &rs,
                                         .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl,
                                         .renderPass = rp};
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    /* record, submit, wait */
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                  .framebuffer = fb, .renderArea = sc, .clearValueCount = 1,
                                  .pClearValues = &clear};
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
    CHECK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull));

    /* check: the triangle covers the image; colour is barycentric in (x, y) */
    uint8_t* px;
    CHECK(vkMapMemory(dev, bmem, 0, W * H * 4, 0, (void**)&px));
    int bad = 0;
    const int pts[][2] = {{0, 0}, {W - 1, 0}, {0, H - 1}, {W / 2, H / 2}, {W / 4, H / 4}};
    for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; i++) {
        int x = pts[i][0], y = pts[i][1];
        const uint8_t* p = px + (y * W + x) * 4;
        float fx = (x + 0.5f) / W * 2.0f - 1.0f, fy = (y + 0.5f) / H * 2.0f - 1.0f;
        float g = (fx + 1.0f) / 4.0f, b = (fy + 1.0f) / 4.0f, r = 1.0f - g - b;
        int er = (int)(r * 255 + 0.5f), eg = (int)(g * 255 + 0.5f), eb = (int)(b * 255 + 0.5f);
        int ok = abs(p[0] - er) <= 3 && abs(p[1] - eg) <= 3 && abs(p[2] - eb) <= 3 && p[3] == 255;
        bad += !ok;
        printf("pixel (%2d,%2d) = %3d %3d %3d %3d, expected %3d %3d %3d 255  %s\n", x, y, p[0], p[1], p[2], p[3],
               er, eg, eb, ok ? "ok" : "WRONG");
    }
    /* optional: show the result, centred, on the framebuffer given as argv[1] */
    if (argc > 1) {
        int fd = open(argv[1], O_RDWR);
        if (fd < 0) {
            perror(argv[1]);
        } else {
            int x0 = (FB_W - W) / 2, y0 = (FB_H - H) / 2, y;
            for (y = 0; y < H; y++)
                if (pwrite(fd, px + (size_t)y * W * 4, W * 4, ((off_t)(y0 + y) * FB_W + x0) * 4) != W * 4) {
                    perror("pwrite");
                    break;
                }
            close(fd);
            printf("copied %dx%d to %s at (%d,%d), %d rows\n", W, H, argv[1], x0, y0, y);
        }
    }
    vkUnmapMemory(dev, bmem);
    printf("%s\n", bad ? "FAIL" : "PASS: rendered and read back a triangle");

    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyShaderModule(dev, fs, NULL);
    vkDestroyFramebuffer(dev, fb, NULL);
    vkDestroyRenderPass(dev, rp, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, bmem, NULL);
    vkDestroyImageView(dev, view, NULL);
    vkDestroyImage(dev, img, NULL);
    vkFreeMemory(dev, imem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return bad ? 1 : 0;
}
