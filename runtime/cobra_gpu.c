/*
 * Cobra GPU compute runtime.
 *
 * Vendor-neutral: everything here goes through the Vulkan loader
 * (libvulkan.so.1, dlopen'd at runtime), which is the one compute API with a
 * real ICD on NVIDIA, AMD, Intel, and (via MoltenVK) Apple hardware. No
 * vulkan.h is required to build Cobra - we hand-declare exactly the ABI
 * surface used below. Every struct layout here was validated field-for-field
 * (offsetof + sizeof) against the real Khronos vulkan_core.h during
 * development; anyone touching this file should re-validate the same way
 * rather than trusting memory.
 *
 * cobra_gpu_available()/cobra_gpu_device_count() only need instance
 * creation + physical device enumeration. cobra_gpu_selftest() goes further:
 * it runs one full compute round trip (upload -> dispatch a real SPIR-V
 * shader -> readback) and checks the result, proving actual GPU execution
 * works end to end, not just device detection.
 */
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <dlfcn.h>

/* ---- Hand-declared Vulkan ABI (validated against vulkan_core.h) ---- */

typedef int32_t CVkResult;
typedef uint64_t CVkDeviceSize;
typedef uint32_t CVkFlags;
typedef void *CVkInstance;
typedef void *CVkPhysicalDevice;
typedef void *CVkDevice;
typedef void *CVkQueue;
typedef struct CVkBuffer_T *CVkBuffer;
typedef struct CVkDeviceMemory_T *CVkDeviceMemory;
typedef struct CVkShaderModule_T *CVkShaderModule;
typedef struct CVkDescriptorSetLayout_T *CVkDescriptorSetLayout;
typedef struct CVkDescriptorPool_T *CVkDescriptorPool;
typedef struct CVkDescriptorSet_T *CVkDescriptorSet;
typedef struct CVkPipelineLayout_T *CVkPipelineLayout;
typedef struct CVkPipeline_T *CVkPipeline;
typedef struct CVkCommandPool_T *CVkCommandPool;
typedef struct CVkCommandBuffer_T *CVkCommandBuffer;
typedef struct CVkFence_T *CVkFence;

typedef struct { uint32_t width, height, depth; } CVkExtent3D;

typedef struct {
    uint32_t sType; const void *pNext;
    const char *pApplicationName; uint32_t applicationVersion;
    const char *pEngineName; uint32_t engineVersion;
    uint32_t apiVersion;
} CVkApplicationInfo;

typedef struct {
    uint32_t sType; const void *pNext; CVkFlags flags;
    const CVkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} CVkInstanceCreateInfo;

typedef struct {
    uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    unsigned char reserved_tail[2048]; /* VkPhysicalDeviceLimits + VkPhysicalDeviceSparseProperties: never read */
} CVkPhysicalDeviceProperties;

typedef struct { CVkFlags queueFlags; uint32_t queueCount, timestampValidBits; CVkExtent3D minImageTransferGranularity; } CVkQueueFamilyProperties;

typedef struct { CVkFlags propertyFlags; uint32_t heapIndex; } CVkMemoryType;
typedef struct { CVkDeviceSize size; CVkFlags flags; unsigned char pad4[4]; } CVkMemoryHeap;
typedef struct {
    uint32_t memoryTypeCount; CVkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount; CVkMemoryHeap memoryHeaps[16];
} CVkPhysicalDeviceMemoryProperties;

typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; uint32_t queueFamilyIndex; uint32_t queueCount; const float *pQueuePriorities; } CVkDeviceQueueCreateInfo;
typedef struct {
    uint32_t sType; const void *pNext; CVkFlags flags;
    uint32_t queueCreateInfoCount; const CVkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} CVkDeviceCreateInfo;

typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; CVkDeviceSize size; CVkFlags usage; uint32_t sharingMode; uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices; } CVkBufferCreateInfo;
typedef struct { CVkDeviceSize size, alignment; uint32_t memoryTypeBits; } CVkMemoryRequirements;
typedef struct { uint32_t sType; const void *pNext; CVkDeviceSize allocationSize; uint32_t memoryTypeIndex; } CVkMemoryAllocateInfo;

typedef struct { uint32_t binding, descriptorType, descriptorCount; CVkFlags stageFlags; const void *pImmutableSamplers; } CVkDescriptorSetLayoutBinding;
typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; uint32_t bindingCount; const CVkDescriptorSetLayoutBinding *pBindings; } CVkDescriptorSetLayoutCreateInfo;
typedef struct { uint32_t type; uint32_t descriptorCount; } CVkDescriptorPoolSize;
typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; uint32_t maxSets; uint32_t poolSizeCount; const CVkDescriptorPoolSize *pPoolSizes; } CVkDescriptorPoolCreateInfo;
typedef struct { uint32_t sType; const void *pNext; CVkDescriptorPool descriptorPool; uint32_t descriptorSetCount; const CVkDescriptorSetLayout *pSetLayouts; } CVkDescriptorSetAllocateInfo;
typedef struct { CVkBuffer buffer; CVkDeviceSize offset, range; } CVkDescriptorBufferInfo;
typedef struct {
    uint32_t sType; const void *pNext; CVkDescriptorSet dstSet; uint32_t dstBinding, dstArrayElement, descriptorCount; uint32_t descriptorType;
    const void *pImageInfo; const CVkDescriptorBufferInfo *pBufferInfo; const void *pTexelBufferView;
} CVkWriteDescriptorSet;

typedef struct { CVkFlags stageFlags; uint32_t offset, size; } CVkPushConstantRange;
typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; uint32_t setLayoutCount; const CVkDescriptorSetLayout *pSetLayouts; uint32_t pushConstantRangeCount; const CVkPushConstantRange *pPushConstantRanges; } CVkPipelineLayoutCreateInfo;

typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; size_t codeSize; const uint32_t *pCode; } CVkShaderModuleCreateInfo;
typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; uint32_t stage; CVkShaderModule module; const char *pName; const void *pSpecializationInfo; } CVkPipelineShaderStageCreateInfo;
typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; CVkPipelineShaderStageCreateInfo stage; CVkPipelineLayout layout; CVkPipeline basePipelineHandle; int32_t basePipelineIndex; } CVkComputePipelineCreateInfo;

typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; uint32_t queueFamilyIndex; } CVkCommandPoolCreateInfo;
typedef struct { uint32_t sType; const void *pNext; CVkCommandPool commandPool; uint32_t level; uint32_t commandBufferCount; } CVkCommandBufferAllocateInfo;
typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; const void *pInheritanceInfo; } CVkCommandBufferBeginInfo;
typedef struct {
    uint32_t sType; const void *pNext;
    uint32_t waitSemaphoreCount; const void *pWaitSemaphores; const CVkFlags *pWaitDstStageMask;
    uint32_t commandBufferCount; const CVkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount; const void *pSignalSemaphores;
} CVkSubmitInfo;
typedef struct { uint32_t sType; const void *pNext; CVkFlags flags; } CVkFenceCreateInfo;
typedef struct { uint32_t sType; const void *pNext; CVkFlags srcAccessMask; CVkFlags dstAccessMask; } CVkMemoryBarrier;

#define CVK_STRUCTURE_TYPE_APPLICATION_INFO 0
#define CVK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define CVK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO 2
#define CVK_STRUCTURE_TYPE_DEVICE_CREATE_INFO 3
#define CVK_STRUCTURE_TYPE_SUBMIT_INFO 4
#define CVK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO 5
#define CVK_STRUCTURE_TYPE_FENCE_CREATE_INFO 8
#define CVK_STRUCTURE_TYPE_BUFFER_CREATE_INFO 12
#define CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO 32
#define CVK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO 33
#define CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO 34
#define CVK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET 35
#define CVK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO 18
#define CVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO 29
#define CVK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO 30
#define CVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO 16
#define CVK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO 39
#define CVK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO 40
#define CVK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO 42

#define CVK_API_VERSION_1_0 (1u << 22)
#define CVK_PHYSICAL_DEVICE_TYPE_CPU 4
#define CVK_QUEUE_COMPUTE_BIT 0x2u
#define CVK_BUFFER_USAGE_STORAGE_BUFFER_BIT 0x20u
#define CVK_SHARING_MODE_EXCLUSIVE 0u
#define CVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 0x2u
#define CVK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x4u
#define CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER 7u
#define CVK_SHADER_STAGE_COMPUTE_BIT 0x20u
#define CVK_PIPELINE_BIND_POINT_COMPUTE 1u
#define CVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT 0x800u
#define CVK_ACCESS_SHADER_WRITE_BIT 0x40u
#define CVK_ACCESS_SHADER_READ_BIT 0x20u
#define CVK_STRUCTURE_TYPE_MEMORY_BARRIER 46
#define CVK_COMMAND_BUFFER_LEVEL_PRIMARY 0u
#define CVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT 0x1u
#define CVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 0x2u
#define CVK_SUCCESS 0

/* out[i] = in[i] * 2.0, local_size_x = 64, binding 0 = in (readonly),
   binding 1 = out (writeonly), push constant = element count (uint32).
   Compiled once via glslc from a 6-line GLSL compute shader; committing the
   words directly means Cobra's own build never needs a shader compiler. */
static const uint32_t cobra_gpu_double_spv[] = {
    119734787,65536,851979,50,0,131089,1,393227,1,1280527431,1685353262,808793134,0,196622,0,1,393231,5,4,1852399981,0,11,393232,4,17,64,1,1,196611,2,450,655364,1197427783,1279741775,1885560645,1953718128,1600482425,1701734764,1919509599,1769235301,25974,524292,1197427783,1279741775,1852399429,1685417059,1768185701,1952671090,6649449,262149,4,1852399981,0,196613,8,105,524293,11,1197436007,1633841004,1986939244,1952539503,1231974249,68,262149,17,1634885968,29549,327686,17,0,1853189987,116,262149,19,1634886000,29549,262149,32,1114928463,26229,327686,32,0,1635017060,0,262149,34,1886680431,0,262149,37,1967287881,102,327686,37,0,1635017060,0,196613,39,7368297,262215,11,11,28,196679,17,2,327752,17,0,35,0,262215,31,6,4,196679,32,3,262216,32,0,25,327752,32,0,35,0,196679,34,25,262215,34,33,1,262215,34,34,0,262215,36,6,4,196679,37,3,262216,37,0,24,327752,37,0,35,0,196679,39,24,262215,39,33,0,262215,39,34,0,262215,49,11,25,131091,2,196641,3,2,262165,6,32,0,262176,7,7,6,262167,9,6,3,262176,10,1,9,262203,10,11,1,262187,6,12,0,262176,13,1,6,196638,17,6,262176,18,9,17,262203,18,19,9,262165,20,32,1,262187,20,21,0,262176,22,9,6,131092,25,196630,30,32,196637,31,30,196638,32,31,262176,33,2,32,262203,33,34,2,196637,36,30,196638,37,36,262176,38,2,37,262203,38,39,2,262176,41,2,30,262187,30,44,1073741824,262187,6,47,64,262187,6,48,1,393260,9,49,47,48,48,327734,2,4,0,3,131320,5,262203,7,8,7,327745,13,14,11,12,262205,6,15,14,196670,8,15,262205,6,16,8,327745,22,23,19,21,262205,6,24,23,327854,25,26,16,24,196855,28,0,262394,26,27,28,131320,27,65789,131320,28,262205,6,35,8,262205,6,40,8,393281,41,42,39,21,40,262205,30,43,42,327813,30,45,43,44,393281,41,46,34,21,35,196670,46,45,65789,65592
};

/* C[M,N] = A[M,K] * B[K,N], optionally + bias[N] then ReLU. local_size 8x8,
   bindings 0=A 1=B 2=C 3=bias (readonly, always bound even when unused so the
   descriptor set stays a fixed shape), push constants = {M,N,K,add_bias}.
   Compiled once via glslangValidator from matmul.comp; committing the words
   directly means Cobra's own build never needs a shader compiler. */
static const uint32_t cobra_gpu_matmul_spv[] = {
    119734787,65536,524299,121,0,131089,1,393227,1,1280527431,1685353262,808793134,0,196622,0,1,393231,5,4,1852399981,0,11,393232,4,17,8,8,1,196611,2,450,262149,4,1852399981,0,196613,8,7827314,524293,11,1197436007,1633841004,1986939244,1952539503,1231974249,68,196613,16,7106403,262149,22,1936550212,0,262150,22,0,77,262150,22,1,78,262150,22,2,75,393222,22,3,1600414817,1935763810,0,262149,24,1936550244,0,196613,45,6513505,196613,47,107,262149,59,1097233730,0,262150,59,0,97,196613,61,0,262149,72,1114010946,0,262150,72,0,98,196613,74,0,262149,95,1114010946,7561577,327686,95,0,1935763810,0,196613,97,0,262149,108,1130788162,0,262150,108,0,99,196613,110,0,262215,11,11,28,196679,22,2,327752,22,0,35,0,327752,22,1,35,4,327752,22,2,35,8,327752,22,3,35,12,262215,58,6,4,196679,59,3,262216,59,0,24,327752,59,0,35,0,196679,61,24,262215,61,33,0,262215,61,34,0,262215,71,6,4,196679,72,3,262216,72,0,24,327752,72,0,35,0,196679,74,24,262215,74,33,1,262215,74,34,0,262215,94,6,4,196679,95,3,262216,95,0,24,327752,95,0,35,0,196679,97,24,262215,97,33,3,262215,97,34,0,262215,107,6,4,196679,108,3,262216,108,0,25,327752,108,0,35,0,196679,110,25,262215,110,33,2,262215,110,34,0,262215,120,11,25,131091,2,196641,3,2,262165,6,32,0,262176,7,7,6,262167,9,6,3,262176,10,1,9,262203,10,11,1,262187,6,12,1,262176,13,1,6,262187,6,17,0,131092,20,393246,22,6,6,6,6,262176,23,9,22,262203,23,24,9,262165,25,32,1,262187,25,26,0,262176,27,9,6,262187,25,35,1,196630,43,32,262176,44,7,43,262187,43,46,0,262187,25,54,2,196637,58,43,196638,59,58,262176,60,2,59,262203,60,61,2,262176,68,2,43,196637,71,43,196638,72,71,262176,73,2,72,262203,73,74,2,262187,25,88,3,196637,94,43,196638,95,94,262176,96,2,95,262203,96,97,2,196637,107,43,196638,108,107,262176,109,2,108,262203,109,110,2,262187,6,119,8,393260,9,120,119,119,12,327734,2,4,0,3,131320,5,262203,7,8,7,262203,7,16,7,262203,44,45,7,262203,7,47,7,327745,13,14,11,12,262205,6,15,14,196670,8,15,327745,13,18,11,17,262205,6,19,18,196670,16,19,262205,6,21,8,327745,27,28,24,26,262205,6,29,28,327854,20,30,21,29,262312,20,31,30,196855,33,0,262394,31,32,33,131320,32,262205,6,34,16,327745,27,36,24,35,262205,6,37,36,327854,20,38,34,37,131321,33,131320,33,458997,20,39,30,5,38,32,196855,41,0,262394,39,40,41,131320,40,65789,131320,41,196670,45,46,196670,47,17,131321,48,131320,48,262390,50,51,0,131321,52,131320,52,262205,6,53,47,327745,27,55,24,54,262205,6,56,55,327856,20,57,53,56,262394,57,49,50,131320,49,262205,6,62,8,327745,27,63,24,54,262205,6,64,63,327812,6,65,62,64,262205,6,66,47,327808,6,67,65,66,393281,68,69,61,26,67,262205,43,70,69,262205,6,75,47,327745,27,76,24,35,262205,6,77,76,327812,6,78,75,77,262205,6,79,16,327808,6,80,78,79,393281,68,81,74,26,80,262205,43,82,81,327813,43,83,70,82,262205,43,84,45,327809,43,85,84,83,196670,45,85,131321,51,131320,51,262205,6,86,47,327808,6,87,86,35,196670,47,87,131321,48,131320,50,327745,27,89,24,88,262205,6,90,89,327851,20,91,90,17,196855,93,0,262394,91,92,93,131320,92,262205,6,98,16,393281,68,99,97,26,98,262205,43,100,99,262205,43,101,45,327809,43,102,101,100,196670,45,102,262205,43,103,45,327864,20,104,103,46,196855,106,0,262394,104,105,106,131320,105,196670,45,46,131321,106,131320,106,131321,93,131320,93,262205,6,111,8,327745,27,112,24,35,262205,6,113,112,327812,6,114,111,113,262205,6,115,16,327808,6,116,114,115,262205,43,117,45,393281,68,118,110,26,116,196670,118,117,65789,65592
};

/* Matmul backward: dA[M,K] = dC[M,N] @ B[K,N]^T, i.e. dA[i,k] = sum_n
   dC[i,n]*B[k,n]. Binding 0=dC 1=B 2=dA (writeonly), push={M,N,K}. */
static const uint32_t cobra_gpu_matmul_bwd_da_spv[] = {
    119734787,65536,524299,102,0,131089,1,393227,1,1280527431,1685353262,808793134,0,196622,0,1,393231,5,4,1852399981,0,11,393232,4,17,16,16,1,196611,2,450,262149,4,1852399981,0,196613,8,7827314,524293,11,1197436007,1633841004,1986939244,1952539503,1231974249,68,196613,16,7106403,196613,22,17232,262150,22,0,77,262150,22,1,78,262150,22,2,75,196613,24,25456,196613,45,6513505,196613,47,110,262149,59,1147565378,67,262150,59,0,17252,196613,61,0,262149,72,1114010946,0,262150,72,0,66,196613,74,0,262149,89,1147565378,65,262150,89,0,16740,196613,91,0,262215,11,11,28,196679,22,2,327752,22,0,35,0,327752,22,1,35,4,327752,22,2,35,8,262215,58,6,4,196679,59,3,262216,59,0,24,327752,59,0,35,0,196679,61,24,262215,61,33,0,262215,61,34,0,262215,71,6,4,196679,72,3,262216,72,0,24,327752,72,0,35,0,196679,74,24,262215,74,33,1,262215,74,34,0,262215,88,6,4,196679,89,3,262216,89,0,25,327752,89,0,35,0,196679,91,25,262215,91,33,2,262215,91,34,0,262215,101,11,25,131091,2,196641,3,2,262165,6,32,0,262176,7,7,6,262167,9,6,3,262176,10,1,9,262203,10,11,1,262187,6,12,1,262176,13,1,6,262187,6,17,0,131092,20,327710,22,6,6,6,262176,23,9,22,262203,23,24,9,262165,25,32,1,262187,25,26,0,262176,27,9,6,262187,25,35,2,196630,43,32,262176,44,7,43,262187,43,46,0,262187,25,54,1,196637,58,43,196638,59,58,262176,60,2,59,262203,60,61,2,262176,68,2,43,196637,71,43,196638,72,71,262176,73,2,72,262203,73,74,2,196637,88,43,196638,89,88,262176,90,2,89,262203,90,91,2,262187,6,100,16,393260,9,101,100,100,12,327734,2,4,0,3,131320,5,262203,7,8,7,262203,7,16,7,262203,44,45,7,262203,7,47,7,327745,13,14,11,12,262205,6,15,14,196670,8,15,327745,13,18,11,17,262205,6,19,18,196670,16,19,262205,6,21,8,327745,27,28,24,26,262205,6,29,28,327854,20,30,21,29,262312,20,31,30,196855,33,0,262394,31,32,33,131320,32,262205,6,34,16,327745,27,36,24,35,262205,6,37,36,327854,20,38,34,37,131321,33,131320,33,458997,20,39,30,5,38,32,196855,41,0,262394,39,40,41,131320,40,65789,131320,41,196670,45,46,196670,47,17,131321,48,131320,48,262390,50,51,0,131321,52,131320,52,262205,6,53,47,327745,27,55,24,54,262205,6,56,55,327856,20,57,53,56,262394,57,49,50,131320,49,262205,6,62,8,327745,27,63,24,54,262205,6,64,63,327812,6,65,62,64,262205,6,66,47,327808,6,67,65,66,393281,68,69,61,26,67,262205,43,70,69,262205,6,75,16,327745,27,76,24,54,262205,6,77,76,327812,6,78,75,77,262205,6,79,47,327808,6,80,78,79,393281,68,81,74,26,80,262205,43,82,81,327813,43,83,70,82,262205,43,84,45,327809,43,85,84,83,196670,45,85,131321,51,131320,51,262205,6,86,47,327808,6,87,86,54,196670,47,87,131321,48,131320,50,262205,6,92,8,327745,27,93,24,35,262205,6,94,93,327812,6,95,92,94,262205,6,96,16,327808,6,97,95,96,262205,43,98,45,393281,68,99,91,26,97,196670,99,98,65789,65592
};

/* Matmul backward: dB[K,N] = A[M,K]^T @ dC[M,N], i.e. dB[k,j] = sum_m
   A[m,k]*dC[m,j]. Binding 0=A 1=dC 2=dB (writeonly), push={M,N,K}. */
static const uint32_t cobra_gpu_matmul_bwd_db_spv[] = {
    119734787,65536,524299,102,0,131089,1,393227,1,1280527431,1685353262,808793134,0,196622,0,1,393231,5,4,1852399981,0,11,393232,4,17,16,16,1,196611,2,450,262149,4,1852399981,0,196613,8,7827314,524293,11,1197436007,1633841004,1986939244,1952539503,1231974249,68,196613,16,7106403,196613,22,17232,262150,22,0,77,262150,22,1,78,262150,22,2,75,196613,24,25456,196613,45,6513505,196613,47,109,262149,59,1097233730,0,262150,59,0,65,196613,61,0,262149,72,1147565378,67,262150,72,0,17252,196613,74,0,262149,89,1147565378,66,262150,89,0,16996,196613,91,0,262215,11,11,28,196679,22,2,327752,22,0,35,0,327752,22,1,35,4,327752,22,2,35,8,262215,58,6,4,196679,59,3,262216,59,0,24,327752,59,0,35,0,196679,61,24,262215,61,33,0,262215,61,34,0,262215,71,6,4,196679,72,3,262216,72,0,24,327752,72,0,35,0,196679,74,24,262215,74,33,1,262215,74,34,0,262215,88,6,4,196679,89,3,262216,89,0,25,327752,89,0,35,0,196679,91,25,262215,91,33,2,262215,91,34,0,262215,101,11,25,131091,2,196641,3,2,262165,6,32,0,262176,7,7,6,262167,9,6,3,262176,10,1,9,262203,10,11,1,262187,6,12,1,262176,13,1,6,262187,6,17,0,131092,20,327710,22,6,6,6,262176,23,9,22,262203,23,24,9,262165,25,32,1,262187,25,26,2,262176,27,9,6,262187,25,35,1,196630,43,32,262176,44,7,43,262187,43,46,0,262187,25,54,0,196637,58,43,196638,59,58,262176,60,2,59,262203,60,61,2,262176,68,2,43,196637,71,43,196638,72,71,262176,73,2,72,262203,73,74,2,196637,88,43,196638,89,88,262176,90,2,89,262203,90,91,2,262187,6,100,16,393260,9,101,100,100,12,327734,2,4,0,3,131320,5,262203,7,8,7,262203,7,16,7,262203,44,45,7,262203,7,47,7,327745,13,14,11,12,262205,6,15,14,196670,8,15,327745,13,18,11,17,262205,6,19,18,196670,16,19,262205,6,21,8,327745,27,28,24,26,262205,6,29,28,327854,20,30,21,29,262312,20,31,30,196855,33,0,262394,31,32,33,131320,32,262205,6,34,16,327745,27,36,24,35,262205,6,37,36,327854,20,38,34,37,131321,33,131320,33,458997,20,39,30,5,38,32,196855,41,0,262394,39,40,41,131320,40,65789,131320,41,196670,45,46,196670,47,17,131321,48,131320,48,262390,50,51,0,131321,52,131320,52,262205,6,53,47,327745,27,55,24,54,262205,6,56,55,327856,20,57,53,56,262394,57,49,50,131320,49,262205,6,62,47,327745,27,63,24,26,262205,6,64,63,327812,6,65,62,64,262205,6,66,8,327808,6,67,65,66,393281,68,69,61,54,67,262205,43,70,69,262205,6,75,47,327745,27,76,24,35,262205,6,77,76,327812,6,78,75,77,262205,6,79,16,327808,6,80,78,79,393281,68,81,74,54,80,262205,43,82,81,327813,43,83,70,82,262205,43,84,45,327809,43,85,84,83,196670,45,85,131321,51,131320,51,262205,6,86,47,327808,6,87,86,35,196670,47,87,131321,48,131320,50,262205,6,92,8,327745,27,93,24,35,262205,6,94,93,327812,6,95,92,94,262205,6,96,16,327808,6,97,95,96,262205,43,98,45,393281,68,99,91,54,97,196670,99,98,65789,65592
};

/* data[i] = max(data[i], 0.0) in place. local_size_x=256, binding 0 = data
   (read_write), push constant = element count. */
static const uint32_t cobra_gpu_relu_spv[] = {
    119734787,65536,524299,46,0,131089,1,393227,1,1280527431,1685353262,808793134,0,196622,0,1,393231,5,4,1852399981,0,11,393232,4,17,256,1,1,196611,2,450,262149,4,1852399981,0,196613,8,105,524293,11,1197436007,1633841004,1986939244,1952539503,1231974249,68,196613,17,17232,327686,17,0,1853189987,116,196613,19,25456,196613,32,6714690,327686,32,0,1635017060,0,196613,34,0,262215,11,11,28,196679,17,2,327752,17,0,35,0,262215,31,6,4,196679,32,3,327752,32,0,35,0,262215,34,33,0,262215,34,34,0,262215,45,11,25,131091,2,196641,3,2,262165,6,32,0,262176,7,7,6,262167,9,6,3,262176,10,1,9,262203,10,11,1,262187,6,12,0,262176,13,1,6,196638,17,6,262176,18,9,17,262203,18,19,9,262165,20,32,1,262187,20,21,0,262176,22,9,6,131092,25,196630,30,32,196637,31,30,196638,32,31,262176,33,2,32,262203,33,34,2,262176,37,2,30,262187,30,40,0,262187,6,43,256,262187,6,44,1,393260,9,45,43,44,44,327734,2,4,0,3,131320,5,262203,7,8,7,327745,13,14,11,12,262205,6,15,14,196670,8,15,262205,6,16,8,327745,22,23,19,21,262205,6,24,23,327854,25,26,16,24,196855,28,0,262394,26,27,28,131320,27,65789,131320,28,262205,6,35,8,262205,6,36,8,393281,37,38,34,21,36,262205,30,39,38,458764,30,41,1,40,39,40,393281,37,42,34,21,35,196670,42,41,65789,65592
};

/* Tree reduction: op 0=sum, 1=max. Binding 0 = data (readonly), binding 1 =
   partial (writeonly, one float per workgroup), push constants = {count, op}.
   Caller finishes combining the (small) partial array on the host. */
static const uint32_t cobra_gpu_reduce_spv[] = {
    119734787,65536,524299,140,0,131089,1,393227,1,1280527431,1685353262,808793134,0,196622,0,1,458767,5,4,1852399981,0,11,17,393232,4,17,256,1,1,196611,2,450,262149,4,1852399981,0,196613,8,6580596,524293,11,1281322087,1818321775,1870032457,1769234787,1145663087,0,196613,16,6580583,393221,17,1465871463,1198223983,1886744434,17481,196613,24,17232,327686,24,0,1853189987,116,262150,24,1,28783,196613,26,25456,196613,39,118,262149,49,1231451458,110,327686,49,0,1635017060,0,196613,51,0,262149,75,1952539763,97,196613,82,115,262149,131,1332114754,29813,327686,131,0,1953653104,7102825,196613,133,0,262215,11,11,27,262215,17,11,26,196679,24,2,327752,24,0,35,0,327752,24,1,35,4,262215,48,6,4,196679,49,3,262216,49,0,24,327752,49,0,35,0,196679,51,24,262215,51,33,0,262215,51,34,0,262215,130,6,4,196679,131,3,262216,131,0,25,327752,131,0,35,0,196679,133,25,262215,133,33,1,262215,133,34,0,262215,139,11,25,131091,2,196641,3,2,262165,6,32,0,262176,7,7,6,262167,9,6,3,262176,10,1,9,262203,10,11,1,262187,6,12,0,262176,13,1,6,262203,10,17,1,262187,6,20,256,262174,24,6,6,262176,25,9,24,262203,25,26,9,262165,27,32,1,262187,27,28,1,262176,29,9,6,262187,6,32,1,131092,33,196630,37,32,262176,38,7,37,262187,27,41,0,196637,48,37,196638,49,48,262176,50,2,49,262203,50,51,2,262176,53,2,37,262187,37,57,4286578685,262187,37,71,0,262172,73,37,20,262176,74,4,73,262203,74,75,4,262176,78,4,37,262187,6,80,2,262187,6,81,264,262187,6,83,128,196637,130,37,196638,131,130,262176,132,2,131,262203,132,133,2,393260,9,139,20,32,32,327734,2,4,0,3,131320,5,262203,7,8,7,262203,7,16,7,262203,38,39,7,262203,38,45,7,262203,38,64,7,262203,7,82,7,327745,13,14,11,12,262205,6,15,14,196670,8,15,327745,13,18,17,12,262205,6,19,18,327812,6,21,19,20,262205,6,22,8,327808,6,23,21,22,196670,16,23,327745,29,30,26,28,262205,6,31,30,327850,33,34,31,32,196855,36,0,262394,34,35,59,131320,35,262205,6,40,16,327745,29,42,26,41,262205,6,43,42,327856,33,44,40,43,196855,47,0,262394,44,46,56,131320,46,262205,6,52,16,393281,53,54,51,41,52,262205,37,55,54,196670,45,55,131321,47,131320,56,196670,45,57,131321,47,131320,47,262205,37,58,45,196670,39,58,131321,36,131320,59,262205,6,60,16,327745,29,61,26,41,262205,6,62,61,327856,33,63,60,62,196855,66,0,262394,63,65,70,131320,65,262205,6,67,16,393281,53,68,51,41,67,262205,37,69,68,196670,64,69,131321,66,131320,70,196670,64,71,131321,66,131320,66,262205,37,72,64,196670,39,72,131321,36,131320,36,262205,6,76,8,262205,37,77,39,327745,78,79,75,76,196670,79,77,262368,80,80,81,196670,82,83,131321,84,131320,84,262390,86,87,0,131321,88,131320,88,262205,6,89,82,327852,33,90,89,12,262394,90,85,86,131320,85,262205,6,91,8,262205,6,92,82,327856,33,93,91,92,196855,95,0,262394,93,94,95,131320,94,327745,29,96,26,28,262205,6,97,96,327850,33,98,97,32,196855,100,0,262394,98,99,112,131320,99,262205,6,101,8,262205,6,102,8,327745,78,103,75,102,262205,37,104,103,262205,6,105,8,262205,6,106,82,327808,6,107,105,106,327745,78,108,75,107,262205,37,109,108,458764,37,110,1,40,104,109,327745,78,111,75,101,196670,111,110,131321,100,131320,112,262205,6,113,8,262205,6,114,8,327745,78,115,75,114,262205,37,116,115,262205,6,117,8,262205,6,118,82,327808,6,119,117,118,327745,78,120,75,119,262205,37,121,120,327809,37,122,116,121,327745,78,123,75,113,196670,123,122,131321,100,131320,100,131321,95,131320,95,262368,80,80,81,131321,87,131320,87,262205,6,124,82,327874,6,125,124,32,196670,82,125,131321,84,131320,86,262205,6,126,8,327850,33,127,126,12,196855,129,0,262394,127,128,129,131320,128,327745,13,134,17,12,262205,6,135,134,327745,78,136,75,41,262205,37,137,136,393281,53,138,133,41,135,196670,138,137,131321,129,131320,129,65789,65592
};

typedef struct {
    void *lib;
    CVkInstance instance;
    CVkResult (*CreateInstance)(const CVkInstanceCreateInfo *, const void *, CVkInstance *);
    void (*DestroyInstance)(CVkInstance, const void *);
    CVkResult (*EnumeratePhysicalDevices)(CVkInstance, uint32_t *, CVkPhysicalDevice *);
    void (*GetPhysicalDeviceProperties)(CVkPhysicalDevice, CVkPhysicalDeviceProperties *);
    void (*GetPhysicalDeviceQueueFamilyProperties)(CVkPhysicalDevice, uint32_t *, CVkQueueFamilyProperties *);
    void (*GetPhysicalDeviceMemoryProperties)(CVkPhysicalDevice, CVkPhysicalDeviceMemoryProperties *);
    CVkResult (*CreateDevice)(CVkPhysicalDevice, const CVkDeviceCreateInfo *, const void *, CVkDevice *);
    void (*DestroyDevice)(CVkDevice, const void *);
    void (*GetDeviceQueue)(CVkDevice, uint32_t, uint32_t, CVkQueue *);
    CVkResult (*CreateBuffer)(CVkDevice, const CVkBufferCreateInfo *, const void *, CVkBuffer *);
    void (*DestroyBuffer)(CVkDevice, CVkBuffer, const void *);
    void (*GetBufferMemoryRequirements)(CVkDevice, CVkBuffer, CVkMemoryRequirements *);
    CVkResult (*AllocateMemory)(CVkDevice, const CVkMemoryAllocateInfo *, const void *, CVkDeviceMemory *);
    void (*FreeMemory)(CVkDevice, CVkDeviceMemory, const void *);
    CVkResult (*BindBufferMemory)(CVkDevice, CVkBuffer, CVkDeviceMemory, CVkDeviceSize);
    CVkResult (*MapMemory)(CVkDevice, CVkDeviceMemory, CVkDeviceSize, CVkDeviceSize, CVkFlags, void **);
    void (*UnmapMemory)(CVkDevice, CVkDeviceMemory);
    CVkResult (*CreateShaderModule)(CVkDevice, const CVkShaderModuleCreateInfo *, const void *, CVkShaderModule *);
    void (*DestroyShaderModule)(CVkDevice, CVkShaderModule, const void *);
    CVkResult (*CreateDescriptorSetLayout)(CVkDevice, const CVkDescriptorSetLayoutCreateInfo *, const void *, CVkDescriptorSetLayout *);
    void (*DestroyDescriptorSetLayout)(CVkDevice, CVkDescriptorSetLayout, const void *);
    CVkResult (*CreateDescriptorPool)(CVkDevice, const CVkDescriptorPoolCreateInfo *, const void *, CVkDescriptorPool *);
    void (*DestroyDescriptorPool)(CVkDevice, CVkDescriptorPool, const void *);
    CVkResult (*AllocateDescriptorSets)(CVkDevice, const CVkDescriptorSetAllocateInfo *, CVkDescriptorSet *);
    void (*UpdateDescriptorSets)(CVkDevice, uint32_t, const CVkWriteDescriptorSet *, uint32_t, const void *);
    CVkResult (*CreatePipelineLayout)(CVkDevice, const CVkPipelineLayoutCreateInfo *, const void *, CVkPipelineLayout *);
    void (*DestroyPipelineLayout)(CVkDevice, CVkPipelineLayout, const void *);
    CVkResult (*CreateComputePipelines)(CVkDevice, void *, uint32_t, const CVkComputePipelineCreateInfo *, const void *, CVkPipeline *);
    void (*DestroyPipeline)(CVkDevice, CVkPipeline, const void *);
    CVkResult (*CreateCommandPool)(CVkDevice, const CVkCommandPoolCreateInfo *, const void *, CVkCommandPool *);
    void (*DestroyCommandPool)(CVkDevice, CVkCommandPool, const void *);
    CVkResult (*AllocateCommandBuffers)(CVkDevice, const CVkCommandBufferAllocateInfo *, CVkCommandBuffer *);
    CVkResult (*BeginCommandBuffer)(CVkCommandBuffer, const CVkCommandBufferBeginInfo *);
    CVkResult (*EndCommandBuffer)(CVkCommandBuffer);
    void (*CmdBindPipeline)(CVkCommandBuffer, uint32_t, CVkPipeline);
    void (*CmdBindDescriptorSets)(CVkCommandBuffer, uint32_t, CVkPipelineLayout, uint32_t, uint32_t, const CVkDescriptorSet *, uint32_t, const uint32_t *);
    void (*CmdPushConstants)(CVkCommandBuffer, CVkPipelineLayout, CVkFlags, uint32_t, uint32_t, const void *);
    void (*CmdDispatch)(CVkCommandBuffer, uint32_t, uint32_t, uint32_t);
    void (*CmdPipelineBarrier)(CVkCommandBuffer, CVkFlags, CVkFlags, CVkFlags,
                                uint32_t, const CVkMemoryBarrier *,
                                uint32_t, const void *, uint32_t, const void *);
    CVkResult (*CreateFence)(CVkDevice, const CVkFenceCreateInfo *, const void *, CVkFence *);
    void (*DestroyFence)(CVkDevice, CVkFence, const void *);
    CVkResult (*QueueSubmit)(CVkQueue, uint32_t, const CVkSubmitInfo *, CVkFence);
    CVkResult (*WaitForFences)(CVkDevice, uint32_t, const CVkFence *, uint32_t, uint64_t);
    CVkResult (*ResetFences)(CVkDevice, uint32_t, const CVkFence *);
    CVkResult (*ResetCommandBuffer)(CVkCommandBuffer, CVkFlags);
} CobraVk;

static int cobra_vk_load(CobraVk *vk) {
    memset(vk, 0, sizeof(*vk));
    vk->lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!vk->lib) return 0;
#define LOAD(name) do { vk->name = (void *)dlsym(vk->lib, "vk" #name); if (!vk->name) { dlclose(vk->lib); vk->lib = 0; return 0; } } while (0)
    LOAD(CreateInstance); LOAD(DestroyInstance); LOAD(EnumeratePhysicalDevices);
    LOAD(GetPhysicalDeviceProperties); LOAD(GetPhysicalDeviceQueueFamilyProperties);
    LOAD(GetPhysicalDeviceMemoryProperties); LOAD(CreateDevice); LOAD(DestroyDevice);
    LOAD(GetDeviceQueue); LOAD(CreateBuffer); LOAD(DestroyBuffer);
    LOAD(GetBufferMemoryRequirements); LOAD(AllocateMemory); LOAD(FreeMemory);
    LOAD(BindBufferMemory); LOAD(MapMemory); LOAD(UnmapMemory);
    LOAD(CreateShaderModule); LOAD(DestroyShaderModule);
    LOAD(CreateDescriptorSetLayout); LOAD(DestroyDescriptorSetLayout);
    LOAD(CreateDescriptorPool); LOAD(DestroyDescriptorPool);
    LOAD(AllocateDescriptorSets); LOAD(UpdateDescriptorSets);
    LOAD(CreatePipelineLayout); LOAD(DestroyPipelineLayout);
    LOAD(CreateComputePipelines); LOAD(DestroyPipeline);
    LOAD(CreateCommandPool); LOAD(DestroyCommandPool); LOAD(AllocateCommandBuffers);
    LOAD(BeginCommandBuffer); LOAD(EndCommandBuffer);
    LOAD(CmdBindPipeline); LOAD(CmdBindDescriptorSets); LOAD(CmdPushConstants); LOAD(CmdDispatch);
    LOAD(CmdPipelineBarrier);
    LOAD(CreateFence); LOAD(DestroyFence); LOAD(QueueSubmit); LOAD(WaitForFences);
    LOAD(ResetFences); LOAD(ResetCommandBuffer);
#undef LOAD
    CVkApplicationInfo app; memset(&app, 0, sizeof(app));
    app.sType = CVK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "cobra"; app.apiVersion = CVK_API_VERSION_1_0;
    CVkInstanceCreateInfo info; memset(&info, 0, sizeof(info));
    info.sType = CVK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    if (vk->CreateInstance(&info, 0, &vk->instance) != CVK_SUCCESS) { dlclose(vk->lib); vk->lib = 0; return 0; }
    return 1;
}

static void cobra_vk_unload(CobraVk *vk) {
    if (!vk->lib) return;
    if (vk->instance) vk->DestroyInstance(vk->instance, 0);
    dlclose(vk->lib);
    vk->lib = 0;
}

/* Picks the first non-CPU physical device (skips Mesa's llvmpipe software
   rasterizer, which Vulkan otherwise reports as just another device). */
static CVkPhysicalDevice cobra_vk_pick_device(CobraVk *vk) {
    uint32_t count = 0;
    vk->EnumeratePhysicalDevices(vk->instance, &count, 0);
    if (count == 0) return 0;
    if (count > 16) count = 16;
    CVkPhysicalDevice devices[16];
    vk->EnumeratePhysicalDevices(vk->instance, &count, devices);
    for (uint32_t i = 0; i < count; i++) {
        CVkPhysicalDeviceProperties props; memset(&props, 0, sizeof(props));
        vk->GetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType != CVK_PHYSICAL_DEVICE_TYPE_CPU) return devices[i];
    }
    return 0;
}

int64_t cobra_gpu_device_count(void) {
    CobraVk vk;
    if (!cobra_vk_load(&vk)) return 0;
    uint32_t count = 0;
    vk.EnumeratePhysicalDevices(vk.instance, &count, 0);
    if (count == 0) { cobra_vk_unload(&vk); return 0; }
    if (count > 64) count = 64;
    CVkPhysicalDevice devices[64];
    vk.EnumeratePhysicalDevices(vk.instance, &count, devices);
    int64_t hardware_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        CVkPhysicalDeviceProperties props; memset(&props, 0, sizeof(props));
        vk.GetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType != CVK_PHYSICAL_DEVICE_TYPE_CPU) hardware_count++;
    }
    cobra_vk_unload(&vk);
    return hardware_count;
}

int64_t cobra_gpu_available(void) { return cobra_gpu_device_count() > 0 ? 1 : 0; }

/* "Language detects when it's best" policy: below this element count, fixed
   Vulkan dispatch overhead (command buffer submission, fence wait - roughly
   tens of microseconds even on a warm pipeline) outweighs what a simple
   elementwise/reduction kernel saves versus AVX2 on the CPU path Cobra
   already has. 65536 is a conservative, round threshold for that crossover;
   callers with a more expensive per-element kernel can ignore this and
   dispatch to the GPU at any size. Availability is cached after the first
   check (device enumeration is not free) since callers are expected to ask
   this once per kernel invocation. */
#define COBRA_GPU_DISPATCH_THRESHOLD 65536

int64_t cobra_gpu_should_dispatch(int64_t element_count) {
    static int checked = 0, available = 0;
    if (!checked) { available = (int)cobra_gpu_available(); checked = 1; }
    return (available && element_count >= COBRA_GPU_DISPATCH_THRESHOLD) ? 1 : 0;
}

static int32_t cobra_vk_find_memory_type(CobraVk *vk, CVkPhysicalDevice phys,
                                         uint32_t type_bits, CVkFlags want) {
    CVkPhysicalDeviceMemoryProperties mem; memset(&mem, 0, sizeof(mem));
    vk->GetPhysicalDeviceMemoryProperties(phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) return (int32_t)i;
    }
    return -1;
}

/* Runs one full compute round trip on real GPU hardware: allocates two
   host-visible/host-coherent storage buffers (no staging buffer needed for
   this proof - simplicity over performance), uploads a small f32 array,
   dispatches cobra_gpu_double_spv (out[i] = in[i]*2), reads the result back,
   and checks it. Returns 1 iff every element matches. This is a functional
   proof that Cobra can drive real GPU compute end to end, independent of
   any specific vendor's SDK. */
int64_t cobra_gpu_selftest(void) {
    CobraVk vk;
    if (!cobra_vk_load(&vk)) return 0;
    int64_t ok = 0;

    CVkPhysicalDevice phys = cobra_vk_pick_device(&vk);
    if (!phys) goto done_no_device;

    uint32_t qf_count = 0;
    vk.GetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, 0);
    if (qf_count == 0 || qf_count > 16) goto done_no_device;
    CVkQueueFamilyProperties qfs[16];
    vk.GetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, qfs);
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++)
        if (qfs[i].queueFlags & CVK_QUEUE_COMPUTE_BIT) { queue_family = i; break; }
    if (queue_family == UINT32_MAX) goto done_no_device;

    float priority = 1.0f;
    CVkDeviceQueueCreateInfo qinfo; memset(&qinfo, 0, sizeof(qinfo));
    qinfo.sType = CVK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qinfo.queueFamilyIndex = queue_family; qinfo.queueCount = 1; qinfo.pQueuePriorities = &priority;
    CVkDeviceCreateInfo dinfo; memset(&dinfo, 0, sizeof(dinfo));
    dinfo.sType = CVK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dinfo.queueCreateInfoCount = 1; dinfo.pQueueCreateInfos = &qinfo;
    CVkDevice device = 0;
    if (vk.CreateDevice(phys, &dinfo, 0, &device) != CVK_SUCCESS) goto done_no_device;

    CVkQueue queue = 0;
    vk.GetDeviceQueue(device, queue_family, 0, &queue);

    enum { N = 16 };
    CVkDeviceSize buf_size = N * sizeof(float);
    CVkBuffer buf_in = 0, buf_out = 0;
    CVkDeviceMemory mem_in = 0, mem_out = 0;
    CVkShaderModule shader = 0;
    CVkDescriptorSetLayout set_layout = 0;
    CVkDescriptorPool desc_pool = 0;
    CVkDescriptorSet desc_set = 0;
    CVkPipelineLayout pipe_layout = 0;
    CVkPipeline pipeline = 0;
    CVkCommandPool cmd_pool = 0;
    CVkCommandBuffer cmd = 0;
    CVkFence fence = 0;

    CVkBufferCreateInfo bci; memset(&bci, 0, sizeof(bci));
    bci.sType = CVK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = buf_size; bci.usage = CVK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bci.sharingMode = CVK_SHARING_MODE_EXCLUSIVE;
    if (vk.CreateBuffer(device, &bci, 0, &buf_in) != CVK_SUCCESS) goto cleanup_device;
    if (vk.CreateBuffer(device, &bci, 0, &buf_out) != CVK_SUCCESS) goto cleanup_device;

    CVkMemoryRequirements req; memset(&req, 0, sizeof(req));
    vk.GetBufferMemoryRequirements(device, buf_in, &req);
    int32_t mem_type = cobra_vk_find_memory_type(&vk, phys, req.memoryTypeBits,
        CVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CVK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type < 0) goto cleanup_device;

    CVkMemoryAllocateInfo mai; memset(&mai, 0, sizeof(mai));
    mai.sType = CVK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size; mai.memoryTypeIndex = (uint32_t)mem_type;
    if (vk.AllocateMemory(device, &mai, 0, &mem_in) != CVK_SUCCESS) goto cleanup_device;
    if (vk.AllocateMemory(device, &mai, 0, &mem_out) != CVK_SUCCESS) goto cleanup_device;
    vk.BindBufferMemory(device, buf_in, mem_in, 0);
    vk.BindBufferMemory(device, buf_out, mem_out, 0);

    {
        void *mapped = 0;
        vk.MapMemory(device, mem_in, 0, buf_size, 0, &mapped);
        float *values = (float *)mapped;
        for (int i = 0; i < N; i++) values[i] = (float)(i + 1);
        vk.UnmapMemory(device, mem_in);
    }

    CVkShaderModuleCreateInfo smci; memset(&smci, 0, sizeof(smci));
    smci.sType = CVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sizeof(cobra_gpu_double_spv); smci.pCode = cobra_gpu_double_spv;
    if (vk.CreateShaderModule(device, &smci, 0, &shader) != CVK_SUCCESS) goto cleanup_device;

    CVkDescriptorSetLayoutBinding bindings[2];
    memset(bindings, 0, sizeof(bindings));
    for (int i = 0; i < 2; i++) {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT;
    }
    CVkDescriptorSetLayoutCreateInfo dslci; memset(&dslci, 0, sizeof(dslci));
    dslci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2; dslci.pBindings = bindings;
    if (vk.CreateDescriptorSetLayout(device, &dslci, 0, &set_layout) != CVK_SUCCESS) goto cleanup_device;

    CVkDescriptorPoolSize pool_size; memset(&pool_size, 0, sizeof(pool_size));
    pool_size.type = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER; pool_size.descriptorCount = 2;
    CVkDescriptorPoolCreateInfo dpci; memset(&dpci, 0, sizeof(dpci));
    dpci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &pool_size;
    if (vk.CreateDescriptorPool(device, &dpci, 0, &desc_pool) != CVK_SUCCESS) goto cleanup_device;

    CVkDescriptorSetAllocateInfo dsai; memset(&dsai, 0, sizeof(dsai));
    dsai.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = desc_pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &set_layout;
    if (vk.AllocateDescriptorSets(device, &dsai, &desc_set) != CVK_SUCCESS) goto cleanup_device;

    {
        CVkDescriptorBufferInfo buf_infos[2];
        memset(buf_infos, 0, sizeof(buf_infos));
        buf_infos[0].buffer = buf_in; buf_infos[0].range = buf_size;
        buf_infos[1].buffer = buf_out; buf_infos[1].range = buf_size;
        CVkWriteDescriptorSet writes[2];
        memset(writes, 0, sizeof(writes));
        for (int i = 0; i < 2; i++) {
            writes[i].sType = CVK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = desc_set; writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorCount = 1; writes[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buf_infos[i];
        }
        vk.UpdateDescriptorSets(device, 2, writes, 0, 0);
    }

    CVkPushConstantRange pcr; memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT; pcr.size = sizeof(uint32_t);
    CVkPipelineLayoutCreateInfo plci; memset(&plci, 0, sizeof(plci));
    plci.sType = CVK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &set_layout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vk.CreatePipelineLayout(device, &plci, 0, &pipe_layout) != CVK_SUCCESS) goto cleanup_device;

    CVkComputePipelineCreateInfo cpci; memset(&cpci, 0, sizeof(cpci));
    cpci.sType = CVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = CVK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = CVK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shader; cpci.stage.pName = "main";
    cpci.layout = pipe_layout; cpci.basePipelineIndex = -1;
    if (vk.CreateComputePipelines(device, 0, 1, &cpci, 0, &pipeline) != CVK_SUCCESS) goto cleanup_device;

    CVkCommandPoolCreateInfo cpi; memset(&cpi, 0, sizeof(cpi));
    cpi.sType = CVK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpi.queueFamilyIndex = queue_family;
    if (vk.CreateCommandPool(device, &cpi, 0, &cmd_pool) != CVK_SUCCESS) goto cleanup_device;

    CVkCommandBufferAllocateInfo cbai; memset(&cbai, 0, sizeof(cbai));
    cbai.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmd_pool; cbai.level = CVK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    if (vk.AllocateCommandBuffers(device, &cbai, &cmd) != CVK_SUCCESS) goto cleanup_device;

    {
        CVkCommandBufferBeginInfo cbbi; memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = CVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.BeginCommandBuffer(cmd, &cbbi);
        vk.CmdBindPipeline(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vk.CmdBindDescriptorSets(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0, 1, &desc_set, 0, 0);
        uint32_t count_u32 = N;
        vk.CmdPushConstants(cmd, pipe_layout, CVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count_u32), &count_u32);
        vk.CmdDispatch(cmd, (N + 63) / 64, 1, 1);
        vk.EndCommandBuffer(cmd);
    }

    CVkFenceCreateInfo fci; memset(&fci, 0, sizeof(fci));
    fci.sType = CVK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vk.CreateFence(device, &fci, 0, &fence) != CVK_SUCCESS) goto cleanup_device;

    {
        CVkSubmitInfo si; memset(&si, 0, sizeof(si));
        si.sType = CVK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vk.QueueSubmit(queue, 1, &si, fence) != CVK_SUCCESS) goto cleanup_device;
    }
    if (vk.WaitForFences(device, 1, &fence, 1, UINT64_MAX) != CVK_SUCCESS) goto cleanup_device;

    {
        void *mapped = 0;
        vk.MapMemory(device, mem_out, 0, buf_size, 0, &mapped);
        float *values = (float *)mapped;
        int all_match = 1;
        for (int i = 0; i < N; i++) {
            float expected = (float)(i + 1) * 2.0f;
            if (values[i] != expected) { all_match = 0; break; }
        }
        vk.UnmapMemory(device, mem_out);
        ok = all_match ? 1 : 0;
    }

cleanup_device:
    if (fence) vk.DestroyFence(device, fence, 0);
    if (cmd_pool) vk.DestroyCommandPool(device, cmd_pool, 0);
    if (pipeline) vk.DestroyPipeline(device, pipeline, 0);
    if (pipe_layout) vk.DestroyPipelineLayout(device, pipe_layout, 0);
    if (desc_pool) vk.DestroyDescriptorPool(device, desc_pool, 0);
    if (set_layout) vk.DestroyDescriptorSetLayout(device, set_layout, 0);
    if (shader) vk.DestroyShaderModule(device, shader, 0);
    if (mem_in) vk.FreeMemory(device, mem_in, 0);
    if (mem_out) vk.FreeMemory(device, mem_out, 0);
    if (buf_in) vk.DestroyBuffer(device, buf_in, 0);
    if (buf_out) vk.DestroyBuffer(device, buf_out, 0);
    if (device) vk.DestroyDevice(device, 0);
done_no_device:
    cobra_vk_unload(&vk);
    return ok;
}

/* Persistent matmul context: the loader, instance, device, queue, shader
   module, descriptor set layout, pipeline layout, and pipeline are all
   reusable across calls and expensive to (re)create (device creation alone
   is milliseconds), so they are built once, lazily, on the first matmul call
   and then kept alive for the life of the process - the same pattern CUDA's
   own context/module caching uses. Only the per-call buffers and descriptor
   set, which depend on M/N/K, are created and torn down each call. */
static struct {
    int ready;      /* 1 once init succeeded, -1 once init was tried and failed */
    CobraVk vk;
    CVkPhysicalDevice phys;
    CVkDevice device;
    CVkQueue queue;
    uint32_t queue_family;
    CVkShaderModule shader;
    CVkDescriptorSetLayout set_layout;
    CVkPipelineLayout pipe_layout;
    CVkPipeline pipeline;
    CVkCommandPool cmd_pool;
} g_mm;

/* Shared boilerplate for every persistent kernel context below: load the
   loader, pick a device, find a compute queue family, create the logical
   device + queue + command pool. Each kernel (matmul, relu, reduce) still
   owns its own shader/pipeline, but there is no reason to open three
   separate Vulkan devices in one process. */
static int cobra_vk_init_device(CobraVk *vk, CVkPhysicalDevice *phys_out,
                                 CVkDevice *device_out, CVkQueue *queue_out,
                                 uint32_t *queue_family_out, CVkCommandPool *cmd_pool_out) {
    if (!cobra_vk_load(vk)) return 0;
    CVkPhysicalDevice phys = cobra_vk_pick_device(vk);
    if (!phys) return 0;

    uint32_t qf_count = 0;
    vk->GetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, 0);
    if (qf_count == 0 || qf_count > 16) return 0;
    CVkQueueFamilyProperties qfs[16];
    vk->GetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, qfs);
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++)
        if (qfs[i].queueFlags & CVK_QUEUE_COMPUTE_BIT) { queue_family = i; break; }
    if (queue_family == UINT32_MAX) return 0;

    float priority = 1.0f;
    CVkDeviceQueueCreateInfo qinfo; memset(&qinfo, 0, sizeof(qinfo));
    qinfo.sType = CVK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qinfo.queueFamilyIndex = queue_family; qinfo.queueCount = 1; qinfo.pQueuePriorities = &priority;
    CVkDeviceCreateInfo dinfo; memset(&dinfo, 0, sizeof(dinfo));
    dinfo.sType = CVK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dinfo.queueCreateInfoCount = 1; dinfo.pQueueCreateInfos = &qinfo;
    CVkDevice device = 0;
    if (vk->CreateDevice(phys, &dinfo, 0, &device) != CVK_SUCCESS) return 0;
    CVkQueue queue = 0;
    vk->GetDeviceQueue(device, queue_family, 0, &queue);

    CVkCommandPoolCreateInfo cpi; memset(&cpi, 0, sizeof(cpi));
    cpi.sType = CVK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpi.queueFamilyIndex = queue_family;
    /* Individually-resettable command buffers: the per-kernel persistent
       command buffer (see g_user_kernels) is re-recorded and resubmitted on
       every call rather than allocated fresh each time. */
    cpi.flags = CVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    CVkCommandPool cmd_pool = 0;
    if (vk->CreateCommandPool(device, &cpi, 0, &cmd_pool) != CVK_SUCCESS) { vk->DestroyDevice(device, 0); return 0; }

    *phys_out = phys; *device_out = device; *queue_out = queue;
    *queue_family_out = queue_family; *cmd_pool_out = cmd_pool;
    return 1;
}

static int cobra_gpu_matmul_ctx_init(void) {
    if (g_mm.ready) return g_mm.ready == 1;
    g_mm.ready = -1;
    if (!cobra_vk_init_device(&g_mm.vk, &g_mm.phys, &g_mm.device, &g_mm.queue,
                               &g_mm.queue_family, &g_mm.cmd_pool)) return 0;
    CobraVk *vk = &g_mm.vk;
    uint32_t queue_family = g_mm.queue_family;

    CVkShaderModuleCreateInfo smci; memset(&smci, 0, sizeof(smci));
    smci.sType = CVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sizeof(cobra_gpu_matmul_spv); smci.pCode = cobra_gpu_matmul_spv;
    if (vk->CreateShaderModule(g_mm.device, &smci, 0, &g_mm.shader) != CVK_SUCCESS) goto fail;

    CVkDescriptorSetLayoutBinding bindings[4];
    memset(bindings, 0, sizeof(bindings));
    for (int i = 0; i < 4; i++) {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT;
    }
    CVkDescriptorSetLayoutCreateInfo dslci; memset(&dslci, 0, sizeof(dslci));
    dslci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 4; dslci.pBindings = bindings;
    if (vk->CreateDescriptorSetLayout(g_mm.device, &dslci, 0, &g_mm.set_layout) != CVK_SUCCESS) goto fail;

    CVkPushConstantRange pcr; memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT; pcr.size = 4 * sizeof(uint32_t);
    CVkPipelineLayoutCreateInfo plci; memset(&plci, 0, sizeof(plci));
    plci.sType = CVK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &g_mm.set_layout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vk->CreatePipelineLayout(g_mm.device, &plci, 0, &g_mm.pipe_layout) != CVK_SUCCESS) goto fail;

    CVkComputePipelineCreateInfo cpci; memset(&cpci, 0, sizeof(cpci));
    cpci.sType = CVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = CVK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = CVK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = g_mm.shader; cpci.stage.pName = "main";
    cpci.layout = g_mm.pipe_layout; cpci.basePipelineIndex = -1;
    if (vk->CreateComputePipelines(g_mm.device, 0, 1, &cpci, 0, &g_mm.pipeline) != CVK_SUCCESS) goto fail;
    (void)queue_family;

    g_mm.ready = 1;
    return 1;
fail:
    return 0;
}

static CVkBuffer cobra_mm_make_buffer(CVkDeviceSize size, CVkDeviceMemory *mem_out) {
    CobraVk *vk = &g_mm.vk;
    CVkBuffer buf = 0;
    CVkBufferCreateInfo bci; memset(&bci, 0, sizeof(bci));
    bci.sType = CVK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = CVK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bci.sharingMode = CVK_SHARING_MODE_EXCLUSIVE;
    if (vk->CreateBuffer(g_mm.device, &bci, 0, &buf) != CVK_SUCCESS) return 0;
    CVkMemoryRequirements req; memset(&req, 0, sizeof(req));
    vk->GetBufferMemoryRequirements(g_mm.device, buf, &req);
    int32_t mem_type = cobra_vk_find_memory_type(vk, g_mm.phys, req.memoryTypeBits,
        CVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CVK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type < 0) { vk->DestroyBuffer(g_mm.device, buf, 0); return 0; }
    CVkMemoryAllocateInfo mai; memset(&mai, 0, sizeof(mai));
    mai.sType = CVK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size; mai.memoryTypeIndex = (uint32_t)mem_type;
    if (vk->AllocateMemory(g_mm.device, &mai, 0, mem_out) != CVK_SUCCESS) { vk->DestroyBuffer(g_mm.device, buf, 0); return 0; }
    vk->BindBufferMemory(g_mm.device, buf, *mem_out, 0);
    return buf;
}

/* C[M,N] = A[M,K] @ B[K,N], optionally + bias[N] then ReLU (add_bias != 0),
   dispatched on whichever GPU cobra_vk_pick_device found - any Vulkan ICD,
   not just NVIDIA. Falls back to returning 0 (caller keeps its existing AVX2
   result) on any failure: no device, driver rejects a request, host runs out
   of GPU-visible memory, etc. Buffers are host-visible/host-coherent (no
   staging buffer) for simplicity; on discrete GPUs a staging+device-local
   path would be faster but is not required for correctness. */
int64_t cobra_gpu_matmul_f32(const float *a, const float *b, float *c,
                              int64_t M, int64_t N, int64_t K,
                              const float *bias, int64_t add_bias) {
    if (M <= 0 || N <= 0 || K <= 0) return 0;
    if (!cobra_gpu_matmul_ctx_init()) return 0;
    CobraVk *vk = &g_mm.vk;

    CVkDeviceSize sz_a = (CVkDeviceSize)(M * K) * sizeof(float);
    CVkDeviceSize sz_b = (CVkDeviceSize)(K * N) * sizeof(float);
    CVkDeviceSize sz_c = (CVkDeviceSize)(M * N) * sizeof(float);
    CVkDeviceSize sz_bias = add_bias ? (CVkDeviceSize)N * sizeof(float) : sizeof(float);

    CVkDeviceMemory mem_a = 0, mem_b = 0, mem_c = 0, mem_bias = 0;
    CVkBuffer buf_a = cobra_mm_make_buffer(sz_a, &mem_a);
    CVkBuffer buf_b = cobra_mm_make_buffer(sz_b, &mem_b);
    CVkBuffer buf_c = cobra_mm_make_buffer(sz_c, &mem_c);
    CVkBuffer buf_bias = cobra_mm_make_buffer(sz_bias, &mem_bias);
    CVkDescriptorPool desc_pool = 0;
    CVkDescriptorSet desc_set = 0;
    CVkCommandBuffer cmd = 0;
    CVkFence fence = 0;
    int64_t ok = 0;
    if (!buf_a || !buf_b || !buf_c || !buf_bias) goto cleanup;

    {
        void *mapped = 0;
        vk->MapMemory(g_mm.device, mem_a, 0, sz_a, 0, &mapped); memcpy(mapped, a, (size_t)sz_a); vk->UnmapMemory(g_mm.device, mem_a);
        vk->MapMemory(g_mm.device, mem_b, 0, sz_b, 0, &mapped); memcpy(mapped, b, (size_t)sz_b); vk->UnmapMemory(g_mm.device, mem_b);
        if (add_bias) {
            vk->MapMemory(g_mm.device, mem_bias, 0, sz_bias, 0, &mapped);
            memcpy(mapped, bias, (size_t)sz_bias);
            vk->UnmapMemory(g_mm.device, mem_bias);
        }
    }

    {
        CVkDescriptorPoolSize pool_size; memset(&pool_size, 0, sizeof(pool_size));
        pool_size.type = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER; pool_size.descriptorCount = 4;
        CVkDescriptorPoolCreateInfo dpci; memset(&dpci, 0, sizeof(dpci));
        dpci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &pool_size;
        if (vk->CreateDescriptorPool(g_mm.device, &dpci, 0, &desc_pool) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkDescriptorSetAllocateInfo dsai; memset(&dsai, 0, sizeof(dsai));
        dsai.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = desc_pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &g_mm.set_layout;
        if (vk->AllocateDescriptorSets(g_mm.device, &dsai, &desc_set) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkDescriptorBufferInfo buf_infos[4]; memset(buf_infos, 0, sizeof(buf_infos));
        buf_infos[0].buffer = buf_a; buf_infos[0].range = sz_a;
        buf_infos[1].buffer = buf_b; buf_infos[1].range = sz_b;
        buf_infos[2].buffer = buf_c; buf_infos[2].range = sz_c;
        buf_infos[3].buffer = buf_bias; buf_infos[3].range = sz_bias;
        CVkWriteDescriptorSet writes[4]; memset(writes, 0, sizeof(writes));
        for (int i = 0; i < 4; i++) {
            writes[i].sType = CVK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = desc_set; writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorCount = 1; writes[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buf_infos[i];
        }
        vk->UpdateDescriptorSets(g_mm.device, 4, writes, 0, 0);
    }

    {
        CVkCommandBufferAllocateInfo cbai; memset(&cbai, 0, sizeof(cbai));
        cbai.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_mm.cmd_pool; cbai.level = CVK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        if (vk->AllocateCommandBuffers(g_mm.device, &cbai, &cmd) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkCommandBufferBeginInfo cbbi; memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = CVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk->BeginCommandBuffer(cmd, &cbbi);
        vk->CmdBindPipeline(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_mm.pipeline);
        vk->CmdBindDescriptorSets(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_mm.pipe_layout, 0, 1, &desc_set, 0, 0);
        uint32_t dims[4] = { (uint32_t)M, (uint32_t)N, (uint32_t)K, (uint32_t)(add_bias ? 1 : 0) };
        vk->CmdPushConstants(cmd, g_mm.pipe_layout, CVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dims), dims);
        vk->CmdDispatch(cmd, ((uint32_t)N + 7) / 8, ((uint32_t)M + 7) / 8, 1);
        vk->EndCommandBuffer(cmd);
    }

    {
        CVkFenceCreateInfo fci; memset(&fci, 0, sizeof(fci));
        fci.sType = CVK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vk->CreateFence(g_mm.device, &fci, 0, &fence) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkSubmitInfo si; memset(&si, 0, sizeof(si));
        si.sType = CVK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vk->QueueSubmit(g_mm.queue, 1, &si, fence) != CVK_SUCCESS) goto cleanup;
    }
    if (vk->WaitForFences(g_mm.device, 1, &fence, 1, UINT64_MAX) != CVK_SUCCESS) goto cleanup;

    {
        void *mapped = 0;
        vk->MapMemory(g_mm.device, mem_c, 0, sz_c, 0, &mapped);
        memcpy(c, mapped, (size_t)sz_c);
        vk->UnmapMemory(g_mm.device, mem_c);
    }
    ok = 1;

cleanup:
    if (fence) vk->DestroyFence(g_mm.device, fence, 0);
    /* cmd is freed implicitly when desc_pool's sibling pool (cmd_pool) is
       eventually reset/destroyed at process exit; per-call command buffers
       are cheap enough on top of a persistent pool that explicit per-call
       freeing is unnecessary. */
    if (desc_pool) vk->DestroyDescriptorPool(g_mm.device, desc_pool, 0);
    if (mem_a) vk->FreeMemory(g_mm.device, mem_a, 0);
    if (mem_b) vk->FreeMemory(g_mm.device, mem_b, 0);
    if (mem_c) vk->FreeMemory(g_mm.device, mem_c, 0);
    if (mem_bias) vk->FreeMemory(g_mm.device, mem_bias, 0);
    if (buf_a) vk->DestroyBuffer(g_mm.device, buf_a, 0);
    if (buf_b) vk->DestroyBuffer(g_mm.device, buf_b, 0);
    if (buf_c) vk->DestroyBuffer(g_mm.device, buf_c, 0);
    if (buf_bias) vk->DestroyBuffer(g_mm.device, buf_bias, 0);
    return ok;
}

/* ---- matmul backward: dA = dC @ B^T, dB = A^T @ dC. The two shapes are
   different reductions (dA is M x K, dB is K x N), so they're two separate
   shaders/pipelines/dispatches rather than one - same pattern as running
   two ordinary matmuls, just with transposed indexing baked into each
   shader instead of materializing a transposed copy of A or B. Shares the
   g_mm device (already initialized by a forward matmul call, or lazily
   initialized here if backward is called first) but owns its own
   shader/pipeline pair since the bindings differ (3 buffers, not 4). ---- */
static struct {
    int ready;
    CVkShaderModule shader_da, shader_db;
    CVkDescriptorSetLayout set_layout;   /* same 3-binding layout for both */
    CVkPipelineLayout pipe_layout_da, pipe_layout_db;
    CVkPipeline pipeline_da, pipeline_db;
} g_mm_bwd;

static int cobra_gpu_matmul_bwd_ctx_init(void) {
    if (g_mm_bwd.ready) return g_mm_bwd.ready == 1;
    g_mm_bwd.ready = -1;
    if (!cobra_gpu_matmul_ctx_init()) return 0; /* shares g_mm's device/queue/cmd_pool */
    CobraVk *vk = &g_mm.vk;

    CVkShaderModuleCreateInfo smci_da; memset(&smci_da, 0, sizeof(smci_da));
    smci_da.sType = CVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci_da.codeSize = sizeof(cobra_gpu_matmul_bwd_da_spv); smci_da.pCode = cobra_gpu_matmul_bwd_da_spv;
    if (vk->CreateShaderModule(g_mm.device, &smci_da, 0, &g_mm_bwd.shader_da) != CVK_SUCCESS) goto fail;

    CVkShaderModuleCreateInfo smci_db; memset(&smci_db, 0, sizeof(smci_db));
    smci_db.sType = CVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci_db.codeSize = sizeof(cobra_gpu_matmul_bwd_db_spv); smci_db.pCode = cobra_gpu_matmul_bwd_db_spv;
    if (vk->CreateShaderModule(g_mm.device, &smci_db, 0, &g_mm_bwd.shader_db) != CVK_SUCCESS) goto fail;

    CVkDescriptorSetLayoutBinding bindings[3]; memset(bindings, 0, sizeof(bindings));
    for (int i = 0; i < 3; i++) {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT;
    }
    CVkDescriptorSetLayoutCreateInfo dslci; memset(&dslci, 0, sizeof(dslci));
    dslci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 3; dslci.pBindings = bindings;
    if (vk->CreateDescriptorSetLayout(g_mm.device, &dslci, 0, &g_mm_bwd.set_layout) != CVK_SUCCESS) goto fail;

    CVkPushConstantRange pcr; memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT; pcr.size = 3 * sizeof(uint32_t);
    CVkPipelineLayoutCreateInfo plci; memset(&plci, 0, sizeof(plci));
    plci.sType = CVK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &g_mm_bwd.set_layout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vk->CreatePipelineLayout(g_mm.device, &plci, 0, &g_mm_bwd.pipe_layout_da) != CVK_SUCCESS) goto fail;
    if (vk->CreatePipelineLayout(g_mm.device, &plci, 0, &g_mm_bwd.pipe_layout_db) != CVK_SUCCESS) goto fail;

    CVkComputePipelineCreateInfo cpci_da; memset(&cpci_da, 0, sizeof(cpci_da));
    cpci_da.sType = CVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci_da.stage.sType = CVK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci_da.stage.stage = CVK_SHADER_STAGE_COMPUTE_BIT;
    cpci_da.stage.module = g_mm_bwd.shader_da; cpci_da.stage.pName = "main";
    cpci_da.layout = g_mm_bwd.pipe_layout_da; cpci_da.basePipelineIndex = -1;
    if (vk->CreateComputePipelines(g_mm.device, 0, 1, &cpci_da, 0, &g_mm_bwd.pipeline_da) != CVK_SUCCESS) goto fail;

    CVkComputePipelineCreateInfo cpci_db; memset(&cpci_db, 0, sizeof(cpci_db));
    cpci_db.sType = CVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci_db.stage.sType = CVK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci_db.stage.stage = CVK_SHADER_STAGE_COMPUTE_BIT;
    cpci_db.stage.module = g_mm_bwd.shader_db; cpci_db.stage.pName = "main";
    cpci_db.layout = g_mm_bwd.pipe_layout_db; cpci_db.basePipelineIndex = -1;
    if (vk->CreateComputePipelines(g_mm.device, 0, 1, &cpci_db, 0, &g_mm_bwd.pipeline_db) != CVK_SUCCESS) goto fail;

    g_mm_bwd.ready = 1;
    return 1;
fail:
    return 0;
}

/* Runs one 3-buffer-binding dispatch (dA or dB) against g_mm_bwd's shared
   device. `pipeline`/`pipe_layout` select which of the two shaders runs;
   `bufs[3]`/`sizes[3]` are (in0, in1, out) already sized for that shader's
   own shape; `groups_x`/`groups_y` is the dispatch grid (16x16 workgroups,
   matching the shaders' local_size). Returns 1 on success, 0 on any
   failure - caller keeps whatever `out` already held. */
static int64_t cobra_gpu_matmul_bwd_dispatch(CVkPipeline pipeline, CVkPipelineLayout pipe_layout,
                                              const float *in0, const float *in1, float *out,
                                              CVkDeviceSize sz0, CVkDeviceSize sz1, CVkDeviceSize sz_out,
                                              const uint32_t push[3], uint32_t groups_x, uint32_t groups_y) {
    CobraVk *vk = &g_mm.vk;
    CVkBuffer buf0 = 0, buf1 = 0, buf_out = 0;
    CVkDeviceMemory mem0 = 0, mem1 = 0, mem_out = 0;
    CVkDescriptorPool desc_pool = 0;
    CVkDescriptorSet desc_set = 0;
    CVkCommandBuffer cmd = 0;
    CVkFence fence = 0;
    int64_t ok = 0;

    buf0 = cobra_mm_make_buffer(sz0, &mem0);
    buf1 = cobra_mm_make_buffer(sz1, &mem1);
    buf_out = cobra_mm_make_buffer(sz_out, &mem_out);
    if (!buf0 || !buf1 || !buf_out) goto cleanup;

    {
        void *mapped = 0;
        vk->MapMemory(g_mm.device, mem0, 0, sz0, 0, &mapped); memcpy(mapped, in0, (size_t)sz0); vk->UnmapMemory(g_mm.device, mem0);
        vk->MapMemory(g_mm.device, mem1, 0, sz1, 0, &mapped); memcpy(mapped, in1, (size_t)sz1); vk->UnmapMemory(g_mm.device, mem1);
    }
    {
        CVkDescriptorPoolSize pool_size; memset(&pool_size, 0, sizeof(pool_size));
        pool_size.type = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER; pool_size.descriptorCount = 3;
        CVkDescriptorPoolCreateInfo dpci; memset(&dpci, 0, sizeof(dpci));
        dpci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &pool_size;
        if (vk->CreateDescriptorPool(g_mm.device, &dpci, 0, &desc_pool) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkDescriptorSetAllocateInfo dsai; memset(&dsai, 0, sizeof(dsai));
        dsai.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = desc_pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &g_mm_bwd.set_layout;
        if (vk->AllocateDescriptorSets(g_mm.device, &dsai, &desc_set) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkDescriptorBufferInfo buf_infos[3]; memset(buf_infos, 0, sizeof(buf_infos));
        buf_infos[0].buffer = buf0; buf_infos[0].range = sz0;
        buf_infos[1].buffer = buf1; buf_infos[1].range = sz1;
        buf_infos[2].buffer = buf_out; buf_infos[2].range = sz_out;
        CVkWriteDescriptorSet writes[3]; memset(writes, 0, sizeof(writes));
        for (int i = 0; i < 3; i++) {
            writes[i].sType = CVK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = desc_set; writes[i].dstBinding = (uint32_t)i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buf_infos[i];
        }
        vk->UpdateDescriptorSets(g_mm.device, 3, writes, 0, 0);
    }
    {
        CVkCommandBufferAllocateInfo cbai; memset(&cbai, 0, sizeof(cbai));
        cbai.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_mm.cmd_pool; cbai.level = CVK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        if (vk->AllocateCommandBuffers(g_mm.device, &cbai, &cmd) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkCommandBufferBeginInfo cbbi; memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = CVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk->BeginCommandBuffer(cmd, &cbbi);
        vk->CmdBindPipeline(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vk->CmdBindDescriptorSets(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0, 1, &desc_set, 0, 0);
        vk->CmdPushConstants(cmd, pipe_layout, CVK_SHADER_STAGE_COMPUTE_BIT, 0, 3 * sizeof(uint32_t), push);
        vk->CmdDispatch(cmd, groups_x, groups_y, 1);
        vk->EndCommandBuffer(cmd);
    }
    {
        CVkFenceCreateInfo fci; memset(&fci, 0, sizeof(fci));
        fci.sType = CVK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vk->CreateFence(g_mm.device, &fci, 0, &fence) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkSubmitInfo si; memset(&si, 0, sizeof(si));
        si.sType = CVK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vk->QueueSubmit(g_mm.queue, 1, &si, fence) != CVK_SUCCESS) goto cleanup;
    }
    if (vk->WaitForFences(g_mm.device, 1, &fence, 1, UINT64_MAX) != CVK_SUCCESS) goto cleanup;
    {
        void *mapped = 0;
        vk->MapMemory(g_mm.device, mem_out, 0, sz_out, 0, &mapped);
        memcpy(out, mapped, (size_t)sz_out);
        vk->UnmapMemory(g_mm.device, mem_out);
    }
    ok = 1;

cleanup:
    if (fence) vk->DestroyFence(g_mm.device, fence, 0);
    if (desc_pool) vk->DestroyDescriptorPool(g_mm.device, desc_pool, 0);
    if (mem0) vk->FreeMemory(g_mm.device, mem0, 0);
    if (mem1) vk->FreeMemory(g_mm.device, mem1, 0);
    if (mem_out) vk->FreeMemory(g_mm.device, mem_out, 0);
    if (buf0) vk->DestroyBuffer(g_mm.device, buf0, 0);
    if (buf1) vk->DestroyBuffer(g_mm.device, buf1, 0);
    if (buf_out) vk->DestroyBuffer(g_mm.device, buf_out, 0);
    return ok;
}

/* Standard matmul gradient: for C[M,N] = A[M,K] @ B[K,N],
   dA[M,K] = dC[M,N] @ B[K,N]^T and dB[K,N] = A[M,K]^T @ dC[M,N]. Both `da`
   and `db` are always written on success (pass a dummy 1-element buffer
   and treat it as unused if a caller only needs one side, e.g. a frozen
   embedding layer that never updates its input). Returns 0 on any failure
   (no device, OOM, driver reject) - caller's existing buffers are
   untouched, same convention as cobra_gpu_matmul_f32. */
int64_t cobra_gpu_matmul_backward_f32(const float *a, const float *b, const float *dc,
                                       float *da, float *db,
                                       int64_t M, int64_t N, int64_t K) {
    if (M <= 0 || N <= 0 || K <= 0) return 0;
    if (!cobra_gpu_matmul_bwd_ctx_init()) return 0;

    uint32_t push[3] = { (uint32_t)M, (uint32_t)N, (uint32_t)K };
    CVkDeviceSize sz_a = (CVkDeviceSize)(M * K) * sizeof(float);
    CVkDeviceSize sz_b = (CVkDeviceSize)(K * N) * sizeof(float);
    CVkDeviceSize sz_c = (CVkDeviceSize)(M * N) * sizeof(float);

    /* dA[M,K]: dispatch grid is K columns x M rows (matches the da shader's
       gl_GlobalInvocationID.x -> col in [0,K), .y -> row in [0,M)). */
    if (!cobra_gpu_matmul_bwd_dispatch(g_mm_bwd.pipeline_da, g_mm_bwd.pipe_layout_da,
                                        dc, b, da, sz_c, sz_b, sz_a, push,
                                        (uint32_t)((K + 15) / 16), (uint32_t)((M + 15) / 16))) return 0;
    /* dB[K,N]: dispatch grid is N columns x K rows. */
    if (!cobra_gpu_matmul_bwd_dispatch(g_mm_bwd.pipeline_db, g_mm_bwd.pipe_layout_db,
                                        a, dc, db, sz_a, sz_c, sz_b, push,
                                        (uint32_t)((N + 15) / 16), (uint32_t)((K + 15) / 16))) return 0;
    return 1;
}

/* ---- relu_f32: in-place elementwise max(x, 0) over an entire buffer ---- */

static struct {
    int ready;
    CobraVk vk;
    CVkPhysicalDevice phys;
    CVkDevice device;
    CVkQueue queue;
    uint32_t queue_family;
    CVkShaderModule shader;
    CVkDescriptorSetLayout set_layout;
    CVkPipelineLayout pipe_layout;
    CVkPipeline pipeline;
    CVkCommandPool cmd_pool;
} g_relu;

static int cobra_gpu_relu_ctx_init(void) {
    if (g_relu.ready) return g_relu.ready == 1;
    g_relu.ready = -1;
    if (!cobra_vk_init_device(&g_relu.vk, &g_relu.phys, &g_relu.device, &g_relu.queue,
                               &g_relu.queue_family, &g_relu.cmd_pool)) return 0;
    CobraVk *vk = &g_relu.vk;

    CVkShaderModuleCreateInfo smci; memset(&smci, 0, sizeof(smci));
    smci.sType = CVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sizeof(cobra_gpu_relu_spv); smci.pCode = cobra_gpu_relu_spv;
    if (vk->CreateShaderModule(g_relu.device, &smci, 0, &g_relu.shader) != CVK_SUCCESS) return 0;

    CVkDescriptorSetLayoutBinding binding; memset(&binding, 0, sizeof(binding));
    binding.binding = 0; binding.descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1; binding.stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT;
    CVkDescriptorSetLayoutCreateInfo dslci; memset(&dslci, 0, sizeof(dslci));
    dslci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1; dslci.pBindings = &binding;
    if (vk->CreateDescriptorSetLayout(g_relu.device, &dslci, 0, &g_relu.set_layout) != CVK_SUCCESS) return 0;

    CVkPushConstantRange pcr; memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT; pcr.size = sizeof(uint32_t);
    CVkPipelineLayoutCreateInfo plci; memset(&plci, 0, sizeof(plci));
    plci.sType = CVK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &g_relu.set_layout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vk->CreatePipelineLayout(g_relu.device, &plci, 0, &g_relu.pipe_layout) != CVK_SUCCESS) return 0;

    CVkComputePipelineCreateInfo cpci; memset(&cpci, 0, sizeof(cpci));
    cpci.sType = CVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = CVK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = CVK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = g_relu.shader; cpci.stage.pName = "main";
    cpci.layout = g_relu.pipe_layout; cpci.basePipelineIndex = -1;
    if (vk->CreateComputePipelines(g_relu.device, 0, 1, &cpci, 0, &g_relu.pipeline) != CVK_SUCCESS) return 0;

    g_relu.ready = 1;
    return 1;
}

static CVkBuffer cobra_relu_make_buffer(CVkDeviceSize size, CVkDeviceMemory *mem_out) {
    CobraVk *vk = &g_relu.vk;
    CVkBuffer buf = 0;
    CVkBufferCreateInfo bci; memset(&bci, 0, sizeof(bci));
    bci.sType = CVK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = CVK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bci.sharingMode = CVK_SHARING_MODE_EXCLUSIVE;
    if (vk->CreateBuffer(g_relu.device, &bci, 0, &buf) != CVK_SUCCESS) return 0;
    CVkMemoryRequirements req; memset(&req, 0, sizeof(req));
    vk->GetBufferMemoryRequirements(g_relu.device, buf, &req);
    int32_t mem_type = cobra_vk_find_memory_type(vk, g_relu.phys, req.memoryTypeBits,
        CVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CVK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type < 0) { vk->DestroyBuffer(g_relu.device, buf, 0); return 0; }
    CVkMemoryAllocateInfo mai; memset(&mai, 0, sizeof(mai));
    mai.sType = CVK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size; mai.memoryTypeIndex = (uint32_t)mem_type;
    if (vk->AllocateMemory(g_relu.device, &mai, 0, mem_out) != CVK_SUCCESS) { vk->DestroyBuffer(g_relu.device, buf, 0); return 0; }
    vk->BindBufferMemory(g_relu.device, buf, *mem_out, 0);
    return buf;
}

/* Runs relu in place on `data[0..count)`. Returns 0 on any failure - caller
   keeps its existing (already correct) buffer and falls back to the AVX2
   loop, so a GPU failure here never loses data. */
int64_t cobra_gpu_relu_f32(float *data, int64_t count) {
    if (count <= 0) return 0;
    if (!cobra_gpu_relu_ctx_init()) return 0;
    CobraVk *vk = &g_relu.vk;

    CVkDeviceSize sz = (CVkDeviceSize)count * sizeof(float);
    CVkDeviceMemory mem = 0;
    CVkBuffer buf = cobra_relu_make_buffer(sz, &mem);
    CVkDescriptorPool desc_pool = 0;
    CVkDescriptorSet desc_set = 0;
    CVkCommandBuffer cmd = 0;
    CVkFence fence = 0;
    int64_t ok = 0;
    if (!buf) goto cleanup;

    {
        void *mapped = 0;
        vk->MapMemory(g_relu.device, mem, 0, sz, 0, &mapped);
        memcpy(mapped, data, (size_t)sz);
        vk->UnmapMemory(g_relu.device, mem);
    }

    {
        CVkDescriptorPoolSize pool_size; memset(&pool_size, 0, sizeof(pool_size));
        pool_size.type = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER; pool_size.descriptorCount = 1;
        CVkDescriptorPoolCreateInfo dpci; memset(&dpci, 0, sizeof(dpci));
        dpci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &pool_size;
        if (vk->CreateDescriptorPool(g_relu.device, &dpci, 0, &desc_pool) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkDescriptorSetAllocateInfo dsai; memset(&dsai, 0, sizeof(dsai));
        dsai.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = desc_pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &g_relu.set_layout;
        if (vk->AllocateDescriptorSets(g_relu.device, &dsai, &desc_set) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkDescriptorBufferInfo buf_info; memset(&buf_info, 0, sizeof(buf_info));
        buf_info.buffer = buf; buf_info.range = sz;
        CVkWriteDescriptorSet write; memset(&write, 0, sizeof(write));
        write.sType = CVK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = desc_set; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &buf_info;
        vk->UpdateDescriptorSets(g_relu.device, 1, &write, 0, 0);
    }

    {
        CVkCommandBufferAllocateInfo cbai; memset(&cbai, 0, sizeof(cbai));
        cbai.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_relu.cmd_pool; cbai.level = CVK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        if (vk->AllocateCommandBuffers(g_relu.device, &cbai, &cmd) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkCommandBufferBeginInfo cbbi; memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = CVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk->BeginCommandBuffer(cmd, &cbbi);
        vk->CmdBindPipeline(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_relu.pipeline);
        vk->CmdBindDescriptorSets(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_relu.pipe_layout, 0, 1, &desc_set, 0, 0);
        uint32_t count_u32 = (uint32_t)count;
        vk->CmdPushConstants(cmd, g_relu.pipe_layout, CVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count_u32), &count_u32);
        vk->CmdDispatch(cmd, ((uint32_t)count + 255) / 256, 1, 1);
        vk->EndCommandBuffer(cmd);
    }

    {
        CVkFenceCreateInfo fci; memset(&fci, 0, sizeof(fci));
        fci.sType = CVK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vk->CreateFence(g_relu.device, &fci, 0, &fence) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkSubmitInfo si; memset(&si, 0, sizeof(si));
        si.sType = CVK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vk->QueueSubmit(g_relu.queue, 1, &si, fence) != CVK_SUCCESS) goto cleanup;
    }
    if (vk->WaitForFences(g_relu.device, 1, &fence, 1, UINT64_MAX) != CVK_SUCCESS) goto cleanup;

    {
        void *mapped = 0;
        vk->MapMemory(g_relu.device, mem, 0, sz, 0, &mapped);
        memcpy(data, mapped, (size_t)sz);
        vk->UnmapMemory(g_relu.device, mem);
    }
    ok = 1;

cleanup:
    if (fence) vk->DestroyFence(g_relu.device, fence, 0);
    if (desc_pool) vk->DestroyDescriptorPool(g_relu.device, desc_pool, 0);
    if (mem) vk->FreeMemory(g_relu.device, mem, 0);
    if (buf) vk->DestroyBuffer(g_relu.device, buf, 0);
    return ok;
}

/* ---- sum_f32/max_f32 (and mean_f32, derived on the host): tree reduction
   to one partial float per workgroup, finished on the host. A workgroup
   count in the thousands is still a trivial host-side loop, so there is no
   need for a second GPU pass. ---- */

static struct {
    int ready;
    CobraVk vk;
    CVkPhysicalDevice phys;
    CVkDevice device;
    CVkQueue queue;
    uint32_t queue_family;
    CVkShaderModule shader;
    CVkDescriptorSetLayout set_layout;
    CVkPipelineLayout pipe_layout;
    CVkPipeline pipeline;
    CVkCommandPool cmd_pool;
} g_reduce;

static int cobra_gpu_reduce_ctx_init(void) {
    if (g_reduce.ready) return g_reduce.ready == 1;
    g_reduce.ready = -1;
    if (!cobra_vk_init_device(&g_reduce.vk, &g_reduce.phys, &g_reduce.device, &g_reduce.queue,
                               &g_reduce.queue_family, &g_reduce.cmd_pool)) return 0;
    CobraVk *vk = &g_reduce.vk;

    CVkShaderModuleCreateInfo smci; memset(&smci, 0, sizeof(smci));
    smci.sType = CVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sizeof(cobra_gpu_reduce_spv); smci.pCode = cobra_gpu_reduce_spv;
    if (vk->CreateShaderModule(g_reduce.device, &smci, 0, &g_reduce.shader) != CVK_SUCCESS) return 0;

    CVkDescriptorSetLayoutBinding bindings[2]; memset(bindings, 0, sizeof(bindings));
    for (int i = 0; i < 2; i++) {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT;
    }
    CVkDescriptorSetLayoutCreateInfo dslci; memset(&dslci, 0, sizeof(dslci));
    dslci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2; dslci.pBindings = bindings;
    if (vk->CreateDescriptorSetLayout(g_reduce.device, &dslci, 0, &g_reduce.set_layout) != CVK_SUCCESS) return 0;

    CVkPushConstantRange pcr; memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT; pcr.size = 2 * sizeof(uint32_t);
    CVkPipelineLayoutCreateInfo plci; memset(&plci, 0, sizeof(plci));
    plci.sType = CVK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &g_reduce.set_layout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vk->CreatePipelineLayout(g_reduce.device, &plci, 0, &g_reduce.pipe_layout) != CVK_SUCCESS) return 0;

    CVkComputePipelineCreateInfo cpci; memset(&cpci, 0, sizeof(cpci));
    cpci.sType = CVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = CVK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = CVK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = g_reduce.shader; cpci.stage.pName = "main";
    cpci.layout = g_reduce.pipe_layout; cpci.basePipelineIndex = -1;
    if (vk->CreateComputePipelines(g_reduce.device, 0, 1, &cpci, 0, &g_reduce.pipeline) != CVK_SUCCESS) return 0;

    g_reduce.ready = 1;
    return 1;
}

static CVkBuffer cobra_reduce_make_buffer(CVkDeviceSize size, CVkDeviceMemory *mem_out) {
    CobraVk *vk = &g_reduce.vk;
    CVkBuffer buf = 0;
    CVkBufferCreateInfo bci; memset(&bci, 0, sizeof(bci));
    bci.sType = CVK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = CVK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bci.sharingMode = CVK_SHARING_MODE_EXCLUSIVE;
    if (vk->CreateBuffer(g_reduce.device, &bci, 0, &buf) != CVK_SUCCESS) return 0;
    CVkMemoryRequirements req; memset(&req, 0, sizeof(req));
    vk->GetBufferMemoryRequirements(g_reduce.device, buf, &req);
    int32_t mem_type = cobra_vk_find_memory_type(vk, g_reduce.phys, req.memoryTypeBits,
        CVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CVK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type < 0) { vk->DestroyBuffer(g_reduce.device, buf, 0); return 0; }
    CVkMemoryAllocateInfo mai; memset(&mai, 0, sizeof(mai));
    mai.sType = CVK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size; mai.memoryTypeIndex = (uint32_t)mem_type;
    if (vk->AllocateMemory(g_reduce.device, &mai, 0, mem_out) != CVK_SUCCESS) { vk->DestroyBuffer(g_reduce.device, buf, 0); return 0; }
    vk->BindBufferMemory(g_reduce.device, buf, *mem_out, 0);
    return buf;
}

/* op: 0 = sum, 1 = max. Writes the reduced scalar to *out_result and returns
   1 on success, 0 on any failure (caller falls back to its AVX2 reduction
   loop and never sees a partial/garbage result). */
int64_t cobra_gpu_reduce_f32(const float *data, int64_t count, int64_t op, float *out_result) {
    if (count <= 0) return 0;
    if (!cobra_gpu_reduce_ctx_init()) return 0;
    CobraVk *vk = &g_reduce.vk;

    uint32_t num_groups = (uint32_t)((count + 255) / 256);
    CVkDeviceSize sz_in = (CVkDeviceSize)count * sizeof(float);
    CVkDeviceSize sz_out = (CVkDeviceSize)num_groups * sizeof(float);

    CVkDeviceMemory mem_in = 0, mem_out = 0;
    CVkBuffer buf_in = cobra_reduce_make_buffer(sz_in, &mem_in);
    CVkBuffer buf_out = cobra_reduce_make_buffer(sz_out, &mem_out);
    CVkDescriptorPool desc_pool = 0;
    CVkDescriptorSet desc_set = 0;
    CVkCommandBuffer cmd = 0;
    CVkFence fence = 0;
    int64_t ok = 0;
    if (!buf_in || !buf_out) goto cleanup;

    {
        void *mapped = 0;
        vk->MapMemory(g_reduce.device, mem_in, 0, sz_in, 0, &mapped);
        memcpy(mapped, data, (size_t)sz_in);
        vk->UnmapMemory(g_reduce.device, mem_in);
    }

    {
        CVkDescriptorPoolSize pool_size; memset(&pool_size, 0, sizeof(pool_size));
        pool_size.type = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER; pool_size.descriptorCount = 2;
        CVkDescriptorPoolCreateInfo dpci; memset(&dpci, 0, sizeof(dpci));
        dpci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &pool_size;
        if (vk->CreateDescriptorPool(g_reduce.device, &dpci, 0, &desc_pool) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkDescriptorSetAllocateInfo dsai; memset(&dsai, 0, sizeof(dsai));
        dsai.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = desc_pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &g_reduce.set_layout;
        if (vk->AllocateDescriptorSets(g_reduce.device, &dsai, &desc_set) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkDescriptorBufferInfo buf_infos[2]; memset(buf_infos, 0, sizeof(buf_infos));
        buf_infos[0].buffer = buf_in; buf_infos[0].range = sz_in;
        buf_infos[1].buffer = buf_out; buf_infos[1].range = sz_out;
        CVkWriteDescriptorSet writes[2]; memset(writes, 0, sizeof(writes));
        for (int i = 0; i < 2; i++) {
            writes[i].sType = CVK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = desc_set; writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorCount = 1; writes[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buf_infos[i];
        }
        vk->UpdateDescriptorSets(g_reduce.device, 2, writes, 0, 0);
    }

    {
        CVkCommandBufferAllocateInfo cbai; memset(&cbai, 0, sizeof(cbai));
        cbai.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_reduce.cmd_pool; cbai.level = CVK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        if (vk->AllocateCommandBuffers(g_reduce.device, &cbai, &cmd) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkCommandBufferBeginInfo cbbi; memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = CVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk->BeginCommandBuffer(cmd, &cbbi);
        vk->CmdBindPipeline(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_reduce.pipeline);
        vk->CmdBindDescriptorSets(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_reduce.pipe_layout, 0, 1, &desc_set, 0, 0);
        uint32_t dims[2] = { (uint32_t)count, (uint32_t)op };
        vk->CmdPushConstants(cmd, g_reduce.pipe_layout, CVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dims), dims);
        vk->CmdDispatch(cmd, num_groups, 1, 1);
        vk->EndCommandBuffer(cmd);
    }

    {
        CVkFenceCreateInfo fci; memset(&fci, 0, sizeof(fci));
        fci.sType = CVK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vk->CreateFence(g_reduce.device, &fci, 0, &fence) != CVK_SUCCESS) goto cleanup;
    }
    {
        CVkSubmitInfo si; memset(&si, 0, sizeof(si));
        si.sType = CVK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vk->QueueSubmit(g_reduce.queue, 1, &si, fence) != CVK_SUCCESS) goto cleanup;
    }
    if (vk->WaitForFences(g_reduce.device, 1, &fence, 1, UINT64_MAX) != CVK_SUCCESS) goto cleanup;

    {
        void *mapped = 0;
        vk->MapMemory(g_reduce.device, mem_out, 0, sz_out, 0, &mapped);
        float *partials = (float *)mapped;
        float result = (op == 1) ? -3.402823e38f : 0.0f;
        for (uint32_t i = 0; i < num_groups; i++) {
            result = (op == 1) ? (partials[i] > result ? partials[i] : result) : (result + partials[i]);
        }
        vk->UnmapMemory(g_reduce.device, mem_out);
        *out_result = result;
    }
    ok = 1;

cleanup:
    if (fence) vk->DestroyFence(g_reduce.device, fence, 0);
    if (desc_pool) vk->DestroyDescriptorPool(g_reduce.device, desc_pool, 0);
    if (mem_in) vk->FreeMemory(g_reduce.device, mem_in, 0);
    if (mem_out) vk->FreeMemory(g_reduce.device, mem_out, 0);
    if (buf_in) vk->DestroyBuffer(g_reduce.device, buf_in, 0);
    if (buf_out) vk->DestroyBuffer(g_reduce.device, buf_out, 0);
    return ok;
}

/* ---- Generic launcher for user-defined `@gpu` kernels (see src/gpu_lower.c
   and the generated <program>_gpu_kernels.c wrapper). One shared Vulkan
   device is reused across every user kernel in the process; each distinct
   kernel (identified by its SPIR-V array pointer, which is a stable static
   symbol per @gpu function) gets its own cached shader/pipeline the first
   time it runs. ---- */

#define COBRA_GPU_USER_KERNEL_MAX 32
#define GPU_LOWER_MAX_BUFFERS 8 /* must match GPU_MAX_BUFFERS in src/gpu_lower.c */

static struct {
    int ready; /* 1 once the shared device is up, -1 if it failed */
    CobraVk vk;
    CVkPhysicalDevice phys;
    CVkDevice device;
    CVkQueue queue;
    uint32_t queue_family;
    CVkCommandPool cmd_pool;
} g_user;

static struct {
    const uint32_t *spirv_key;
    CVkShaderModule shader;
    CVkDescriptorSetLayout set_layout;
    CVkPipelineLayout pipe_layout;
    CVkPipeline pipeline;
    uint32_t push_size;
    int nbuffers;
    /* Persistent per-kernel dispatch resources, built once on first use and
       reused on every subsequent call - this is what makes back-to-back
       calls (a training loop's forward/backward/update chain) avoid
       recreating a descriptor pool, allocating a descriptor set, and
       creating a fence on every single dispatch. Only the descriptor set's
       buffer bindings (which buffers/handles are in play) and the command
       buffer's recorded contents change call to call. */
    CVkDescriptorPool desc_pool;
    CVkDescriptorSet desc_set;
    CVkCommandBuffer cmd;
    CVkFence fence;
    /* Which g_batch.generation last recorded a dispatch of this kernel -
       used to reject a second dispatch of the same kernel within one open
       batch (see the batching section below for why that's unsafe). */
    int64_t batch_gen;
} g_user_kernels[COBRA_GPU_USER_KERNEL_MAX];
static int g_user_kernel_count;

static int cobra_gpu_user_device_init(void) {
    if (g_user.ready) return g_user.ready == 1;
    g_user.ready = -1;
    if (!cobra_vk_init_device(&g_user.vk, &g_user.phys, &g_user.device, &g_user.queue,
                               &g_user.queue_family, &g_user.cmd_pool)) return 0;
    g_user.ready = 1;
    return 1;
}

static CVkBuffer cobra_user_make_buffer(CVkDeviceSize size, CVkDeviceMemory *mem_out) {
    CobraVk *vk = &g_user.vk;
    CVkBuffer buf = 0;
    CVkBufferCreateInfo bci; memset(&bci, 0, sizeof(bci));
    bci.sType = CVK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = CVK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bci.sharingMode = CVK_SHARING_MODE_EXCLUSIVE;
    if (vk->CreateBuffer(g_user.device, &bci, 0, &buf) != CVK_SUCCESS) return 0;
    CVkMemoryRequirements req; memset(&req, 0, sizeof(req));
    vk->GetBufferMemoryRequirements(g_user.device, buf, &req);
    int32_t mem_type = cobra_vk_find_memory_type(vk, g_user.phys, req.memoryTypeBits,
        CVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CVK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type < 0) { vk->DestroyBuffer(g_user.device, buf, 0); return 0; }
    CVkMemoryAllocateInfo mai; memset(&mai, 0, sizeof(mai));
    mai.sType = CVK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size; mai.memoryTypeIndex = (uint32_t)mem_type;
    if (vk->AllocateMemory(g_user.device, &mai, 0, mem_out) != CVK_SUCCESS) { vk->DestroyBuffer(g_user.device, buf, 0); return 0; }
    vk->BindBufferMemory(g_user.device, buf, *mem_out, 0);
    return buf;
}

/* Finds (or lazily builds) the cached pipeline for `spirv`. `push_size` is
   the total push-constant block size in bytes and `nbuffers` the number of
   SSBO bindings, matching the layout src/gpu_lower.c emitted (one binding
   per f32[] parameter, in declaration order). */
static int cobra_gpu_user_kernel_slot(const uint32_t *spirv, size_t spirv_words, uint32_t push_size, int nbuffers) {
    for (int i = 0; i < g_user_kernel_count; i++)
        if (g_user_kernels[i].spirv_key == spirv) return i;
    if (g_user_kernel_count >= COBRA_GPU_USER_KERNEL_MAX || nbuffers <= 0 || nbuffers > GPU_LOWER_MAX_BUFFERS) return -1;
    if (!cobra_gpu_user_device_init()) return -1;
    CobraVk *vk = &g_user.vk;
    int slot = g_user_kernel_count;

    CVkShaderModuleCreateInfo smci; memset(&smci, 0, sizeof(smci));
    smci.sType = CVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spirv_words * sizeof(uint32_t); smci.pCode = spirv;
    if (vk->CreateShaderModule(g_user.device, &smci, 0, &g_user_kernels[slot].shader) != CVK_SUCCESS) return -1;

    CVkDescriptorSetLayoutBinding bindings[GPU_LOWER_MAX_BUFFERS]; memset(bindings, 0, sizeof(bindings));
    for (int i = 0; i < nbuffers; i++) {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT;
    }
    CVkDescriptorSetLayoutCreateInfo dslci; memset(&dslci, 0, sizeof(dslci));
    dslci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = (uint32_t)nbuffers; dslci.pBindings = bindings;
    if (vk->CreateDescriptorSetLayout(g_user.device, &dslci, 0, &g_user_kernels[slot].set_layout) != CVK_SUCCESS) return -1;

    CVkPushConstantRange pcr; memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = CVK_SHADER_STAGE_COMPUTE_BIT; pcr.size = push_size;
    CVkPipelineLayoutCreateInfo plci; memset(&plci, 0, sizeof(plci));
    plci.sType = CVK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &g_user_kernels[slot].set_layout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vk->CreatePipelineLayout(g_user.device, &plci, 0, &g_user_kernels[slot].pipe_layout) != CVK_SUCCESS) return -1;

    CVkComputePipelineCreateInfo cpci; memset(&cpci, 0, sizeof(cpci));
    cpci.sType = CVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = CVK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = CVK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = g_user_kernels[slot].shader; cpci.stage.pName = "main";
    cpci.layout = g_user_kernels[slot].pipe_layout; cpci.basePipelineIndex = -1;
    if (vk->CreateComputePipelines(g_user.device, 0, 1, &cpci, 0, &g_user_kernels[slot].pipeline) != CVK_SUCCESS) return -1;

    CVkDescriptorPoolSize pool_size; memset(&pool_size, 0, sizeof(pool_size));
    pool_size.type = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER; pool_size.descriptorCount = (uint32_t)nbuffers;
    CVkDescriptorPoolCreateInfo dpci; memset(&dpci, 0, sizeof(dpci));
    dpci.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &pool_size;
    if (vk->CreateDescriptorPool(g_user.device, &dpci, 0, &g_user_kernels[slot].desc_pool) != CVK_SUCCESS) return -1;

    CVkDescriptorSetAllocateInfo dsai; memset(&dsai, 0, sizeof(dsai));
    dsai.sType = CVK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_user_kernels[slot].desc_pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &g_user_kernels[slot].set_layout;
    if (vk->AllocateDescriptorSets(g_user.device, &dsai, &g_user_kernels[slot].desc_set) != CVK_SUCCESS) return -1;

    CVkCommandBufferAllocateInfo cbai; memset(&cbai, 0, sizeof(cbai));
    cbai.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_user.cmd_pool; cbai.level = CVK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    if (vk->AllocateCommandBuffers(g_user.device, &cbai, &g_user_kernels[slot].cmd) != CVK_SUCCESS) return -1;

    CVkFenceCreateInfo fci; memset(&fci, 0, sizeof(fci));
    fci.sType = CVK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vk->CreateFence(g_user.device, &fci, 0, &g_user_kernels[slot].fence) != CVK_SUCCESS) return -1;

    g_user_kernels[slot].spirv_key = spirv;
    g_user_kernels[slot].push_size = push_size;
    g_user_kernels[slot].nbuffers = nbuffers;
    g_user_kernel_count++;
    return slot;
}

/* Runs a user @gpu kernel over `nbuffers` in-place f32 buffers. `push_blob`/
   `push_blob_size` is the kernel's complete push-constant data, already
   packed by the generated wrapper to match the layout gpu_lower.c emitted
   (one uint length per buffer in declaration order, then each scalar in
   declaration order). The dispatch domain is buffers[0]'s length. Returns 0
   on any failure - the caller (the wrapper function named after the Cobra
   kernel) is expected to have no CPU fallback, since @gpu is an explicit
   user opt-in, not an auto-dispatch heuristic; a failure here is surfaced as
   a nonzero exit rather than silently running wrong code. */
int64_t cobra_gpu_run_kernel_n(const uint32_t *spirv, size_t spirv_words,
                                float **buffers, const int64_t *lens, int64_t nbuffers,
                                const void *push_blob, size_t push_blob_size) {
    if (nbuffers <= 0 || nbuffers > GPU_LOWER_MAX_BUFFERS || lens[0] <= 0 || push_blob_size > 240) return 0;
    int slot = cobra_gpu_user_kernel_slot(spirv, spirv_words, (uint32_t)push_blob_size, (int)nbuffers);
    if (slot < 0) return 0;
    CobraVk *vk = &g_user.vk;

    CVkDeviceSize sizes[GPU_LOWER_MAX_BUFFERS];
    CVkDeviceMemory mems[GPU_LOWER_MAX_BUFFERS]; memset(mems, 0, sizeof(mems));
    CVkBuffer bufs[GPU_LOWER_MAX_BUFFERS]; memset(bufs, 0, sizeof(bufs));
    /* Descriptor set/command buffer/fence are the same persistent per-slot
       resources cobra_gpu_run_kernel_resident reuses - only the underlying
       storage buffers (necessarily fresh each call here, since this path
       uploads/downloads host memory rather than reusing device-resident
       buffers) get created and torn down per call. */
    CVkDescriptorSet desc_set = g_user_kernels[slot].desc_set;
    CVkCommandBuffer cmd = g_user_kernels[slot].cmd;
    CVkFence fence = g_user_kernels[slot].fence;
    int64_t ok = 0;
    bool all_bufs_ok = true;

    for (int i = 0; i < nbuffers; i++) {
        if (lens[i] <= 0) { all_bufs_ok = false; break; }
        sizes[i] = (CVkDeviceSize)lens[i] * sizeof(float);
        bufs[i] = cobra_user_make_buffer(sizes[i], &mems[i]);
        if (!bufs[i]) { all_bufs_ok = false; break; }
        void *mapped = 0;
        vk->MapMemory(g_user.device, mems[i], 0, sizes[i], 0, &mapped);
        memcpy(mapped, buffers[i], (size_t)sizes[i]);
        vk->UnmapMemory(g_user.device, mems[i]);
    }
    if (!all_bufs_ok) goto cleanup;

    {
        CVkDescriptorBufferInfo buf_infos[GPU_LOWER_MAX_BUFFERS]; memset(buf_infos, 0, sizeof(buf_infos));
        CVkWriteDescriptorSet writes[GPU_LOWER_MAX_BUFFERS]; memset(writes, 0, sizeof(writes));
        for (int i = 0; i < nbuffers; i++) {
            buf_infos[i].buffer = bufs[i]; buf_infos[i].range = sizes[i];
            writes[i].sType = CVK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = desc_set; writes[i].dstBinding = (uint32_t)i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buf_infos[i];
        }
        vk->UpdateDescriptorSets(g_user.device, (uint32_t)nbuffers, writes, 0, 0);
    }
    vk->ResetCommandBuffer(cmd, 0);
    {
        CVkCommandBufferBeginInfo cbbi; memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = CVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk->BeginCommandBuffer(cmd, &cbbi);
        vk->CmdBindPipeline(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_user_kernels[slot].pipeline);
        vk->CmdBindDescriptorSets(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_user_kernels[slot].pipe_layout, 0, 1, &desc_set, 0, 0);
        vk->CmdPushConstants(cmd, g_user_kernels[slot].pipe_layout, CVK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)push_blob_size, push_blob);
        vk->CmdDispatch(cmd, ((uint32_t)lens[0] + 255) / 256, 1, 1);
        vk->EndCommandBuffer(cmd);
    }
    vk->ResetFences(g_user.device, 1, &fence);
    {
        CVkSubmitInfo si; memset(&si, 0, sizeof(si));
        si.sType = CVK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vk->QueueSubmit(g_user.queue, 1, &si, fence) != CVK_SUCCESS) goto cleanup;
    }
    if (vk->WaitForFences(g_user.device, 1, &fence, 1, UINT64_MAX) != CVK_SUCCESS) goto cleanup;
    for (int i = 0; i < nbuffers; i++) {
        void *mapped = 0;
        vk->MapMemory(g_user.device, mems[i], 0, sizes[i], 0, &mapped);
        memcpy(buffers[i], mapped, (size_t)sizes[i]);
        vk->UnmapMemory(g_user.device, mems[i]);
    }
    ok = 1;

cleanup:
    for (int i = 0; i < nbuffers; i++) {
        if (mems[i]) vk->FreeMemory(g_user.device, mems[i], 0);
        if (bufs[i]) vk->DestroyBuffer(g_user.device, bufs[i], 0);
    }
    return ok;
}

/* ---- GPU-resident buffers: this is the "avoid the Python tax" piece.
   cobra_gpu_run_kernel_n above uploads, dispatches, and downloads on every
   single call - correct, but it means every kernel invocation in a training
   loop pays a full host<->device round trip even though the data (weights,
   activations) never actually needs to leave the GPU between steps. A
   resident buffer is allocated once, uploaded once, and handed directly to
   as many kernel dispatches as the caller wants (cobra_gpu_run_kernel_
   resident below) with zero copies in between; the caller downloads only
   when it actually needs the result on the host (e.g. to print a loss). ---- */

#define COBRA_GPU_RESIDENT_MAX 256

static struct {
    CVkBuffer buf;
    CVkDeviceMemory mem;
    int64_t len;
    int active;
} g_resident[COBRA_GPU_RESIDENT_MAX];

/* Allocates a device-resident f32 buffer of `count` elements. Returns a
   1-based handle (0 = failure) - callers pass this handle to
   cobra_gpu_upload_f32/cobra_gpu_download_f32/cobra_gpu_run_kernel_resident/
   cobra_gpu_free instead of a host pointer. */
int64_t cobra_gpu_alloc_f32(int64_t count) {
    if (count <= 0 || !cobra_gpu_user_device_init()) return 0;
    for (int i = 0; i < COBRA_GPU_RESIDENT_MAX; i++) {
        if (g_resident[i].active) continue;
        CVkDeviceMemory mem = 0;
        CVkBuffer buf = cobra_user_make_buffer((CVkDeviceSize)count * sizeof(float), &mem);
        if (!buf) return 0;
        g_resident[i].buf = buf; g_resident[i].mem = mem; g_resident[i].len = count; g_resident[i].active = 1;
        return (int64_t)i + 1;
    }
    return 0;
}

static bool cobra_gpu_resident_valid(int64_t handle, int64_t count) {
    if (handle <= 0 || handle > COBRA_GPU_RESIDENT_MAX) return false;
    int idx = (int)handle - 1;
    return g_resident[idx].active && count > 0 && count <= g_resident[idx].len;
}

int64_t cobra_gpu_upload_f32(int64_t handle, const float *host, int64_t count) {
    if (!cobra_gpu_resident_valid(handle, count)) return 0;
    int idx = (int)handle - 1;
    CobraVk *vk = &g_user.vk;
    void *mapped = 0;
    if (vk->MapMemory(g_user.device, g_resident[idx].mem, 0, (CVkDeviceSize)count * sizeof(float), 0, &mapped) != CVK_SUCCESS) return 0;
    memcpy(mapped, host, (size_t)(count * (int64_t)sizeof(float)));
    vk->UnmapMemory(g_user.device, g_resident[idx].mem);
    return 1;
}

int64_t cobra_gpu_download_f32(int64_t handle, float *host, int64_t count) {
    if (!cobra_gpu_resident_valid(handle, count)) return 0;
    int idx = (int)handle - 1;
    CobraVk *vk = &g_user.vk;
    void *mapped = 0;
    if (vk->MapMemory(g_user.device, g_resident[idx].mem, 0, (CVkDeviceSize)count * sizeof(float), 0, &mapped) != CVK_SUCCESS) return 0;
    memcpy(host, mapped, (size_t)(count * (int64_t)sizeof(float)));
    vk->UnmapMemory(g_user.device, g_resident[idx].mem);
    return 1;
}

/* Returns a resident buffer's element count (0 for an invalid/inactive
   handle) - the generated resident kernel wrapper (see main.c) uses this to
   fill in each buffer's push-constant length field, since it only has
   handles on hand, not raw lengths. */
int64_t cobra_gpu_resident_len(int64_t handle) {
    if (handle <= 0 || handle > COBRA_GPU_RESIDENT_MAX) return 0;
    int idx = (int)handle - 1;
    return g_resident[idx].active ? g_resident[idx].len : 0;
}

int64_t cobra_gpu_free_resident(int64_t handle) {
    if (handle <= 0 || handle > COBRA_GPU_RESIDENT_MAX) return 0;
    int idx = (int)handle - 1;
    if (!g_resident[idx].active) return 0;
    CobraVk *vk = &g_user.vk;
    vk->FreeMemory(g_user.device, g_resident[idx].mem, 0);
    vk->DestroyBuffer(g_user.device, g_resident[idx].buf, 0);
    g_resident[idx].active = 0;
    return 1;
}

/* Runs a user @gpu kernel directly over `nbuffers` already-resident buffers
   (handles from cobra_gpu_alloc_f32) - no upload, no download, just bind
   and dispatch. This is the fast path a training loop should use: allocate
   and upload weights/activations once, then call this every step. Dispatch
   domain is handles[0]'s length, same convention as cobra_gpu_run_kernel_n. */
/* ---- Command-buffer batching: groups multiple resident kernel dispatches
   (a training step's forward/backward/update chain, or several such steps)
   into ONE submit + ONE wait instead of one submit+wait per kernel. This is
   the same class of fix as the persistent descriptor set/command
   buffer/fence above, one level up: it removes the CPU<->GPU round trip
   *between* kernels in a chain, not just the Vulkan object churn within a
   single kernel's call.
   Safety: each kernel's persistent descriptor set (see g_user_kernels) is
   shared across calls, so if the SAME kernel were dispatched twice within
   one open batch with different buffer bindings, the second
   UpdateDescriptorSets would silently corrupt the first dispatch's
   already-recorded-but-not-yet-executed commands (Vulkan descriptor sets
   are read at execution time, not at record time). Rather than allow that
   silent corruption, a kernel used twice in the same batch is rejected. ---- */
static struct {
    int active;
    int ready;
    CVkCommandBuffer cmd;
    CVkFence fence;
    int64_t generation;
} g_batch;

static int cobra_gpu_batch_ensure_resources(void) {
    if (g_batch.ready) return 1;
    if (!cobra_gpu_user_device_init()) return 0;
    CobraVk *vk = &g_user.vk;
    CVkCommandBufferAllocateInfo cbai; memset(&cbai, 0, sizeof(cbai));
    cbai.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_user.cmd_pool; cbai.level = CVK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    if (vk->AllocateCommandBuffers(g_user.device, &cbai, &g_batch.cmd) != CVK_SUCCESS) return 0;
    CVkFenceCreateInfo fci; memset(&fci, 0, sizeof(fci));
    fci.sType = CVK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vk->CreateFence(g_user.device, &fci, 0, &g_batch.fence) != CVK_SUCCESS) return 0;
    g_batch.ready = 1;
    return 1;
}

/* Opens a batch: subsequent cobra_gpu_run_kernel_resident calls record into
   a shared command buffer instead of submitting immediately. Returns 0 if a
   batch is already open or the device isn't available. */
int64_t cobra_gpu_batch_begin(void) {
    if (g_batch.active) return 0;
    if (!cobra_gpu_batch_ensure_resources()) return 0;
    CobraVk *vk = &g_user.vk;
    vk->ResetCommandBuffer(g_batch.cmd, 0);
    CVkCommandBufferBeginInfo cbbi; memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = CVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk->BeginCommandBuffer(g_batch.cmd, &cbbi) != CVK_SUCCESS) return 0;
    g_batch.generation++;
    g_batch.active = 1;
    return 1;
}

/* Submits every kernel recorded since gpu_batch_begin() as one command
   buffer and blocks until the whole chain finishes. Returns 0 if no batch
   is open or submission/execution failed. */
int64_t cobra_gpu_batch_end(void) {
    if (!g_batch.active) return 0;
    CobraVk *vk = &g_user.vk;
    g_batch.active = 0;
    if (vk->EndCommandBuffer(g_batch.cmd) != CVK_SUCCESS) return 0;
    vk->ResetFences(g_user.device, 1, &g_batch.fence);
    CVkSubmitInfo si; memset(&si, 0, sizeof(si));
    si.sType = CVK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &g_batch.cmd;
    if (vk->QueueSubmit(g_user.queue, 1, &si, g_batch.fence) != CVK_SUCCESS) return 0;
    if (vk->WaitForFences(g_user.device, 1, &g_batch.fence, 1, UINT64_MAX) != CVK_SUCCESS) return 0;
    return 1;
}

int64_t cobra_gpu_run_kernel_resident(const uint32_t *spirv, size_t spirv_words,
                                       const int64_t *handles, int64_t nbuffers,
                                       const void *push_blob, size_t push_blob_size) {
    if (nbuffers <= 0 || nbuffers > GPU_LOWER_MAX_BUFFERS || push_blob_size > 240) return 0;
    for (int i = 0; i < nbuffers; i++) if (!cobra_gpu_resident_valid(handles[i], 1)) return 0;
    int slot = cobra_gpu_user_kernel_slot(spirv, spirv_words, (uint32_t)push_blob_size, (int)nbuffers);
    if (slot < 0) return 0;
    CobraVk *vk = &g_user.vk;
    CVkDescriptorSet desc_set = g_user_kernels[slot].desc_set;

    if (g_batch.active && g_user_kernels[slot].batch_gen == g_batch.generation) return 0; /* see file-header note */
    if (g_batch.active) g_user_kernels[slot].batch_gen = g_batch.generation;

    /* Every call reuses the same descriptor set built once in
       cobra_gpu_user_kernel_slot - only the buffer bindings (which
       resident handles are in play this call) change. This is what lets a
       training loop's forward/backward/update chain avoid a fresh
       descriptor pool on every single dispatch. */
    {
        CVkDescriptorBufferInfo buf_infos[GPU_LOWER_MAX_BUFFERS]; memset(buf_infos, 0, sizeof(buf_infos));
        CVkWriteDescriptorSet writes[GPU_LOWER_MAX_BUFFERS]; memset(writes, 0, sizeof(writes));
        for (int i = 0; i < nbuffers; i++) {
            int idx = (int)handles[i] - 1;
            buf_infos[i].buffer = g_resident[idx].buf;
            buf_infos[i].range = (CVkDeviceSize)g_resident[idx].len * sizeof(float);
            writes[i].sType = CVK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = desc_set; writes[i].dstBinding = (uint32_t)i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = CVK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buf_infos[i];
        }
        vk->UpdateDescriptorSets(g_user.device, (uint32_t)nbuffers, writes, 0, 0);
    }

    int idx0 = (int)handles[0] - 1;
    if (g_batch.active) {
        /* Record into the shared batch command buffer; no submit/wait here
           - cobra_gpu_batch_end() flushes everything recorded since
           gpu_batch_begin() in one shot. */
        vk->CmdBindPipeline(g_batch.cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_user_kernels[slot].pipeline);
        vk->CmdBindDescriptorSets(g_batch.cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_user_kernels[slot].pipe_layout, 0, 1, &desc_set, 0, 0);
        vk->CmdPushConstants(g_batch.cmd, g_user_kernels[slot].pipe_layout, CVK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)push_blob_size, push_blob);
        vk->CmdDispatch(g_batch.cmd, ((uint32_t)g_resident[idx0].len + 255) / 256, 1, 1);
        /* Vulkan does not guarantee that one compute dispatch's storage-
           buffer writes are visible to the next dispatch in the same
           command buffer without an explicit barrier - back-to-back
           dispatches on the same queue are not implicitly ordered for
           memory visibility, only for command *issue* order. A chained
           forward/backward/update batch depends on exactly that
           visibility, so every batched dispatch is followed by one. */
        CVkMemoryBarrier barrier; memset(&barrier, 0, sizeof(barrier));
        barrier.sType = CVK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = CVK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = CVK_ACCESS_SHADER_READ_BIT | CVK_ACCESS_SHADER_WRITE_BIT;
        vk->CmdPipelineBarrier(g_batch.cmd, CVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, CVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0, 1, &barrier, 0, 0, 0, 0);
        return 1;
    }

    CVkCommandBuffer cmd = g_user_kernels[slot].cmd;
    CVkFence fence = g_user_kernels[slot].fence;
    vk->ResetCommandBuffer(cmd, 0);
    {
        CVkCommandBufferBeginInfo cbbi; memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = CVK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = CVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk->BeginCommandBuffer(cmd, &cbbi);
        vk->CmdBindPipeline(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_user_kernels[slot].pipeline);
        vk->CmdBindDescriptorSets(cmd, CVK_PIPELINE_BIND_POINT_COMPUTE, g_user_kernels[slot].pipe_layout, 0, 1, &desc_set, 0, 0);
        vk->CmdPushConstants(cmd, g_user_kernels[slot].pipe_layout, CVK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)push_blob_size, push_blob);
        vk->CmdDispatch(cmd, ((uint32_t)g_resident[idx0].len + 255) / 256, 1, 1);
        vk->EndCommandBuffer(cmd);
    }

    vk->ResetFences(g_user.device, 1, &fence);
    {
        CVkSubmitInfo si; memset(&si, 0, sizeof(si));
        si.sType = CVK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vk->QueueSubmit(g_user.queue, 1, &si, fence) != CVK_SUCCESS) return 0;
    }
    if (vk->WaitForFences(g_user.device, 1, &fence, 1, UINT64_MAX) != CVK_SUCCESS) return 0;
    return 1;
}
