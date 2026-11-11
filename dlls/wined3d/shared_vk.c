/* Experimental same-process NV12 external-memory sharing.
 * LGPL-2.1-or-later. Keyed access is explicit; legacy access is serialized
 * per WineD3D command. Deliberately excludes cross-process and NT handles. */
#include "wined3d_private.h"
#include "wined3d_vk.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);

struct wined3d_shared_vk
{
    struct list entry;
    HANDLE handle;
    struct wined3d_resource_desc desc;
    GUID device_uuid;
    VkDeviceSize size;
    unsigned int memory_type;
    unsigned int refs;
    UINT64 key;
    struct wined3d_texture_vk *owner;
    bool abandoned;
};

static struct list shared_images = LIST_INIT(shared_images);
static SRWLOCK shared_lock = SRWLOCK_INIT;

static bool sharing_option(const char *name)
{
    char value[2];
    return GetEnvironmentVariableA(name, value, sizeof(value)) == 1 && value[0] == '1';
}

bool wined3d_shared_legacy_enabled(void)
{
    /* Like renderer selection, this experimental option is fixed per process. */
    static LONG enabled = -1;
    LONG value = InterlockedCompareExchange(&enabled, -1, -1);
    if (value == -1)
    {
        value = sharing_option("WINE_ROUVY_NV12_LEGACY");
        InterlockedCompareExchange(&enabled, value, -1);
    }
    return value && wined3d_get_renderer() == WINED3D_RENDERER_VULKAN;
}

bool wined3d_shared_vk_enabled(void)
{
    return wined3d_get_renderer() == WINED3D_RENDERER_VULKAN
            && (sharing_option("WINE_ROUVY_NV12_SHARED") || wined3d_shared_legacy_enabled());
}

bool wined3d_shared_legacy_resource(const struct wined3d_resource *r)
{
    return r && r->type == WINED3D_RTYPE_TEXTURE_2D && r->format->id == WINED3DFMT_NV12_PLANAR
            && (r->usage & (WINED3DUSAGE_SHARED | WINED3DUSAGE_SHARED_KEYED_MUTEX | WINED3DUSAGE_SHARED_NT_HANDLE))
                    == WINED3DUSAGE_SHARED && wined3d_shared_legacy_enabled();
}

bool wined3d_shared_vk_resource_pinned(struct wined3d_resource *r)
{
    return wined3d_get_renderer() == WINED3D_RENDERER_VULKAN
            && r->type == WINED3D_RTYPE_TEXTURE_2D && (r->usage & WINED3DUSAGE_SHARED)
            && wined3d_texture_vk(texture_from_resource(r))->shared;
}

bool wined3d_shared_vk_resource_supported(struct wined3d_resource *r)
{
    return r->type == WINED3D_RTYPE_TEXTURE_2D
            && r->format->id == WINED3DFMT_NV12_PLANAR && wined3d_shared_vk_enabled()
            && !(r->usage & WINED3DUSAGE_SHARED_NT_HANDLE)
            && (((r->usage & WINED3DUSAGE_SHARED_KEYED_MUTEX) && sharing_option("WINE_ROUVY_NV12_SHARED"))
                    || wined3d_shared_legacy_resource(r))
            && r->access == WINED3D_RESOURCE_ACCESS_GPU
            && r->bind_flags == WINED3D_BIND_SHADER_RESOURCE
            && texture_from_resource(r)->level_count == 1 && texture_from_resource(r)->layer_count == 1;
}

/* Called with shared_lock held. A handle is never cast to a pointer. */
static struct wined3d_shared_vk *find_shared(HANDLE handle)
{
    struct wined3d_shared_vk *s;
    LIST_FOR_EACH_ENTRY(s, &shared_images, struct wined3d_shared_vk, entry)
        if (s->handle == handle) return s;
    return NULL;
}

static void shared_barrier(struct wined3d_texture_vk *t, struct wined3d_context_vk *c, bool acquire)
{
    struct wined3d_device_vk *device = wined3d_device_vk(c->c.device);
    const struct wined3d_vk_info *vk_info = c->vk_info;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    VkCommandBuffer cb = wined3d_context_vk_get_command_buffer(c);

    wined3d_context_vk_end_current_render_pass(c);
    b.srcAccessMask = acquire ? 0 : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask = acquire ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : device->graphics_queue.vk_queue_family_index;
    b.dstQueueFamilyIndex = acquire ? device->graphics_queue.vk_queue_family_index : VK_QUEUE_FAMILY_EXTERNAL;
    b.image = t->image.vk_image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = b.subresourceRange.layerCount = 1;
    VK_CALL(vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b));
    wined3d_context_vk_reference_texture(c, t);
}

void wined3d_shared_vk_initial_release(struct wined3d_texture_vk *t, struct wined3d_context_vk *c)
{
    shared_barrier(t, c, false);
}

bool wined3d_shared_vk_create_image(struct wined3d_texture_vk *t,
        struct wined3d_context_vk *c, const VkImageCreateInfo *original)
{
    struct wined3d_adapter_vk *adapter = wined3d_adapter_vk(c->c.device->adapter);
    struct wined3d_device_vk *device = wined3d_device_vk(c->c.device);
    const struct wined3d_vk_info *vk_info = c->vk_info;
    VkPhysicalDeviceExternalImageFormatInfo external = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT};
    VkPhysicalDeviceImageFormatInfo2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &external};
    VkExternalImageFormatProperties external_props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external_props};
    VkExternalMemoryImageCreateInfo image_external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT};
    VkImageCreateInfo image_info = *original;
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    VkExportMemoryAllocateInfo export_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &dedicated,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT};
    VkImportMemoryWin32HandleInfoKHR import_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
            .pNext = &dedicated, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT};
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkMemoryGetWin32HandleInfoKHR get_handle = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT};
    VkMemoryRequirements requirements;
    struct wined3d_shared_vk *s = t->shared;
    VkResult vr;

    if (!wined3d_shared_vk_resource_supported(&t->t.resource) || !vk_info->vk_ops.vkGetMemoryWin32HandleKHR
            || !vk_info->vk_ops.vkGetPhysicalDeviceImageFormatProperties2)
        return false;
    /* Do not silently replace an exported allocation after an unload. */
    if (s && !t->shared_import) return false;
    query.format = original->format;
    query.type = original->imageType;
    query.tiling = original->tiling;
    query.usage = original->usage;
    query.flags = original->flags;
    vr = VK_CALL(vkGetPhysicalDeviceImageFormatProperties2(adapter->physical_device, &query, &properties));
    if (vr != VK_SUCCESS || (external_props.externalMemoryProperties.externalMemoryFeatures
            & (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
            != (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)
            || original->extent.width > properties.imageFormatProperties.maxExtent.width
            || original->extent.height > properties.imageFormatProperties.maxExtent.height)
        return false;
    image_info.pNext = &image_external;
    if ((vr = VK_CALL(vkCreateImage(device->vk_device, &image_info, NULL, &t->image.vk_image))) != VK_SUCCESS)
        return false;
    VK_CALL(vkGetImageMemoryRequirements(device->vk_device, t->image.vk_image, &requirements));
    dedicated.image = t->image.vk_image;
    if (!t->shared_import)
    {
        if (!(s = calloc(1, sizeof(*s)))) goto fail;
        s->refs = 1;
        s->size = requirements.size;
        s->memory_type = wined3d_adapter_vk_get_memory_type_index(adapter,
                requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (s->memory_type == ~0u) goto fail;
        s->device_uuid = adapter->a.device_uuid;
        wined3d_resource_get_desc(&t->t.resource, &s->desc);
    }
    if (requirements.size != s->size || !(requirements.memoryTypeBits & (1u << s->memory_type))) goto fail;
    allocation.allocationSize = s->size;
    allocation.memoryTypeIndex = s->memory_type;
    import_info.handle = s->handle;
    allocation.pNext = t->shared_import ? (void *)&import_info : (void *)&export_info;
    if ((vr = VK_CALL(vkAllocateMemory(device->vk_device, &allocation, NULL, &t->image.vk_memory))) != VK_SUCCESS)
        goto fail;
    if ((vr = VK_CALL(vkBindImageMemory(device->vk_device, t->image.vk_image, t->image.vk_memory, 0))) != VK_SUCCESS)
        goto fail;
    if (!t->shared_import)
    {
        get_handle.memory = t->image.vk_memory;
        if ((vr = VK_CALL(vkGetMemoryWin32HandleKHR(device->vk_device, &get_handle, &s->handle))) != VK_SUCCESS)
            goto fail;
        AcquireSRWLockExclusive(&shared_lock);
        list_add_tail(&shared_images, &s->entry);
        ReleaseSRWLockExclusive(&shared_lock);
        t->shared = s;
    }
    TRACE("NV12 shared image %s memory %s handle %p device %p import %u.\n",
            wine_dbgstr_longlong(t->image.vk_image), wine_dbgstr_longlong(t->image.vk_memory),
            s->handle, device, t->shared_import);
    TRACE("NV12 physical device UUID %s.\n", debugstr_guid(&s->device_uuid));
    return true;
fail:
    VK_CALL(vkDestroyImage(device->vk_device, t->image.vk_image, NULL));
    if (t->image.vk_memory) VK_CALL(vkFreeMemory(device->vk_device, t->image.vk_memory, NULL));
    memset(&t->image, 0, sizeof(t->image));
    if (!t->shared_import) free(s);
    return false;
}

void wined3d_shared_vk_detach(void *object)
{
    struct wined3d_texture_vk *t = object;
    struct wined3d_shared_vk *s = t->shared;
    if (!s) return;
    AcquireSRWLockExclusive(&shared_lock);
    if (s->owner == t) { s->owner = NULL; s->abandoned = true; }
    if (!--s->refs)
    {
        list_remove(&s->entry);
        free(s);
    }
    t->shared = NULL;
    ReleaseSRWLockExclusive(&shared_lock);
}

struct shared_operation
{
    struct wined3d_texture_vk *texture;
    int operation; /* 0 initialize/import, 1 acquire, 2 release */
    HRESULT hr;
};

static void shared_operation_cs(void *object)
{
    struct shared_operation *op = object;
    struct wined3d_texture_vk *t = op->texture;
    struct wined3d_context *context = context_acquire(t->t.resource.device, NULL, 0);
    struct wined3d_context_vk *c = wined3d_context_vk(context);
    struct wined3d_device_vk *device = wined3d_device_vk(context->device);
    const struct wined3d_vk_info *vk_info = c->vk_info;
    VkResult vr;

    op->hr = E_FAIL;
    if (!wined3d_texture_vk_prepare_texture(t, c)) goto done;
    if (op->operation) shared_barrier(t, c, op->operation == 1);
    wined3d_context_vk_submit_command_buffer(c, 0, NULL, NULL, 0, NULL);
    /* Conservative first implementation: completion before publishing the key.
     * This is not a performance implementation. No raw VkDevice is shared. */
    vr = VK_CALL(vkQueueWaitIdle(device->graphics_queue.vk_queue));
    if (vr == VK_SUCCESS)
    {
        if (op->operation == 1)
        {
            wined3d_texture_validate_location(&t->t, 0, WINED3D_LOCATION_TEXTURE_RGB);
            wined3d_texture_invalidate_location(&t->t, 0, ~WINED3D_LOCATION_TEXTURE_RGB);
        }
        op->hr = S_OK;
    }
done:
    context_release(context);
}

static HRESULT run_shared_operation(struct wined3d_texture_vk *t, int operation)
{
    struct shared_operation op = {t, operation, E_FAIL};
    wined3d_cs_init_object(t->t.resource.device->cs, shared_operation_cs, &op);
    wined3d_cs_finish(t->t.resource.device->cs, WINED3D_CS_QUEUE_DEFAULT);
    return op.hr;
}

HRESULT CDECL wined3d_resource_get_shared_handle(struct wined3d_resource *r, HANDLE *handle)
{
    struct wined3d_texture_vk *t;
    HRESULT hr;
    if (!handle) return E_INVALIDARG;
    *handle = NULL;
    if (!wined3d_shared_vk_resource_supported(r)) return E_NOTIMPL;
    t = wined3d_texture_vk(texture_from_resource(r));
    wined3d_mutex_lock();
    hr = run_shared_operation(t, 0);
    if (SUCCEEDED(hr)) *handle = t->shared->handle;
    wined3d_mutex_unlock();
    return hr;
}

HRESULT CDECL wined3d_shared_get_desc(HANDLE handle, struct wined3d_resource_desc *desc)
{
    struct wined3d_shared_vk *s;
    HRESULT hr = E_INVALIDARG;
    if (!wined3d_shared_vk_enabled()) return E_NOTIMPL;
    AcquireSRWLockExclusive(&shared_lock);
    if ((s = find_shared(handle))) { *desc = s->desc; hr = S_OK; }
    ReleaseSRWLockExclusive(&shared_lock);
    return hr;
}

HRESULT CDECL wined3d_resource_import_shared(struct wined3d_resource *r, HANDLE handle)
{
    struct wined3d_shared_vk *s;
    struct wined3d_texture_vk *t;
    HRESULT hr = E_INVALIDARG;
    if (!wined3d_shared_vk_resource_supported(r)) return E_NOTIMPL;
    t = wined3d_texture_vk(texture_from_resource(r));
    wined3d_mutex_lock();
    AcquireSRWLockExclusive(&shared_lock);
    if (!t->shared && !(t->t.flags & WINED3D_TEXTURE_RGB_ALLOCATED) && (s = find_shared(handle))
            && IsEqualGUID(&s->device_uuid, &r->device->adapter->device_uuid)
            && r->width == s->desc.width && r->height == s->desc.height
            && (r->usage & WINED3DUSAGE_SHARED_KEYED_MUTEX) == (s->desc.usage & WINED3DUSAGE_SHARED_KEYED_MUTEX))
    {
        ++s->refs;
        t->shared = s;
        t->shared_import = true;
        hr = S_OK;
    }
    ReleaseSRWLockExclusive(&shared_lock);
    if (SUCCEEDED(hr)) hr = run_shared_operation(t, 0);
    wined3d_mutex_unlock();
    return hr;
}

HRESULT CDECL wined3d_resource_acquire_shared(struct wined3d_resource *r, UINT64 key, DWORD timeout)
{
    struct wined3d_texture_vk *t;
    struct wined3d_shared_vk *s;
    HANDLE handle;
    ULONGLONG start = GetTickCount64();
    HRESULT hr;
    if (FAILED(hr = wined3d_resource_get_shared_handle(r, &handle))) return hr;
    t = wined3d_texture_vk(texture_from_resource(r));
    s = t->shared;
    for (;;)
    {
        AcquireSRWLockExclusive(&shared_lock);
        if (s->abandoned) hr = WAIT_ABANDONED;
        else if (s->owner && s->owner->t.resource.device == r->device) hr = DXGI_ERROR_INVALID_CALL;
        else if (!s->owner && s->key == key) { s->owner = t; hr = S_OK; }
        else hr = WAIT_TIMEOUT;
        ReleaseSRWLockExclusive(&shared_lock);
        if (hr != WAIT_TIMEOUT || (timeout != INFINITE && GetTickCount64() - start >= timeout)) break;
        Sleep(1);
    }
    if (hr != S_OK) return hr;
    wined3d_mutex_lock();
    hr = run_shared_operation(t, 1);
    wined3d_mutex_unlock();
    if (FAILED(hr))
    {
        AcquireSRWLockExclusive(&shared_lock);
        s->abandoned = true;
        s->owner = NULL;
        ReleaseSRWLockExclusive(&shared_lock);
    }
    return hr;
}

HRESULT CDECL wined3d_resource_release_shared(struct wined3d_resource *r, UINT64 key)
{
    struct wined3d_texture_vk *t;
    struct wined3d_shared_vk *s;
    HRESULT hr;
    if (!wined3d_shared_vk_resource_supported(r)) return E_NOTIMPL;
    t = wined3d_texture_vk(texture_from_resource(r));
    if (!(s = t->shared)) return DXGI_ERROR_INVALID_CALL;
    wined3d_mutex_lock();
    AcquireSRWLockExclusive(&shared_lock);
    hr = s->owner == t ? S_OK : DXGI_ERROR_INVALID_CALL;
    ReleaseSRWLockExclusive(&shared_lock);
    if (FAILED(hr)) { wined3d_mutex_unlock(); return hr; }
    hr = run_shared_operation(t, 2);
    AcquireSRWLockExclusive(&shared_lock);
    s->owner = NULL;
    s->key = key;
    if (FAILED(hr)) s->abandoned = true;
    ReleaseSRWLockExclusive(&shared_lock);
    wined3d_mutex_unlock();
    return hr;
}

/* Legacy sharing has no client mutex. Serialize complete commands across all
 * participating CS threads, and return ownership to EXTERNAL before unlocking.
 * This lock is never taken by an API thread waiting for its CS. */
static SRWLOCK legacy_access_lock = SRWLOCK_INIT;

bool wined3d_shared_access_begin(struct wined3d_shared_access *a)
{
    struct wined3d_texture_vk *t;
    struct shared_operation op;
    unsigned int i, j, acquired = 0;

    if (!a->count) return true;
    AcquireSRWLockExclusive(&legacy_access_lock);
    for (i = 0; i < a->count; ++i)
    {
        t = wined3d_texture_vk(texture_from_resource(a->resources[i]));
        op.texture = t;
        op.operation = 0;
        shared_operation_cs(&op);
        if (FAILED(op.hr) || !t->shared || t->shared->abandoned) goto fail;
        /* Distinct aliases of one allocation in the same command need a more
         * general aliasing model. The two NV12 plane views use the same t. */
        for (j = 0; j < i; ++j)
            if (wined3d_texture_vk(texture_from_resource(a->resources[j]))->shared == t->shared)
                goto fail;
    }
    for (i = 0; i < a->count; ++i)
    {
        op.texture = wined3d_texture_vk(texture_from_resource(a->resources[i]));
        op.operation = 1;
        shared_operation_cs(&op);
        if (FAILED(op.hr)) goto fail;
        ++acquired;
    }
    return true;
fail:
    ERR("Cannot acquire legacy NV12 resources for %s; command rejected.\n", a->operation);
    while (acquired)
    {
        op.texture = wined3d_texture_vk(texture_from_resource(a->resources[--acquired]));
        op.operation = 2;
        shared_operation_cs(&op);
    }
    ReleaseSRWLockExclusive(&legacy_access_lock);
    return false;
}

void wined3d_shared_access_end(struct wined3d_shared_access *a)
{
    struct wined3d_texture_vk *t;
    struct wined3d_shared_vk *s;
    struct shared_operation op;
    unsigned int i;

    if (!a->count) return;
    for (i = a->count; i; --i)
    {
        t = wined3d_texture_vk(texture_from_resource(a->resources[i - 1]));
        s = t->shared;
        op.texture = t;
        op.operation = 2;
        shared_operation_cs(&op);
        if (FAILED(op.hr))
        {
            AcquireSRWLockExclusive(&shared_lock);
            s->abandoned = true;
            ReleaseSRWLockExclusive(&shared_lock);
            ERR("Cannot release legacy NV12 resource %p after %s.\n", s->handle, a->operation);
            continue;
        }

    }
    ReleaseSRWLockExclusive(&legacy_access_lock);
}
