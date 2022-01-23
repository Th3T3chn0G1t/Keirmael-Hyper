#include "ultra.h"
#include "ultra_protocol.h"
#include "elf/elf.h"
#include "common/bug.h"
#include "common/cpuid.h"
#include "common/helpers.h"
#include "common/constants.h"
#include "common/format.h"
#include "common/log.h"
#include "filesystem/filesystem_table.h"
#include "allocator.h"
#include "virtual_memory.h"
#include "handover.h"

#undef MSG_FMT
#define MSG_FMT(msg) "ULTRA-PROT: " msg

struct binary_options {
    struct full_path path;
    bool allocate_anywhere;
};

static void get_binary_options(struct config *cfg, struct loadable_entry *le, struct binary_options *opts)
{
    struct value binary_val;
    struct string_view string_path;

    CFG_MANDATORY_GET_ONE_OF(VALUE_STRING | VALUE_OBJECT, cfg, le, SV("binary"), &binary_val);

    if (value_is_object(&binary_val)) {
        CFG_MANDATORY_GET(string, cfg, &binary_val, SV("path"), &string_path);
        cfg_get_bool(cfg, &binary_val, SV("allocate-anywhere"), &opts->allocate_anywhere);
    } else {
        string_path = binary_val.as_string;
    }

    if (!parse_path(string_path, &opts->path))
        oops("invalid binary path %pSV", &string_path);
}

static void module_load(struct config *cfg, struct value *module_value, struct ultra_module_info_attribute *attrs)
{
    struct full_path path;
    const struct fs_entry *fse;
    struct string_view str_path, module_name = { 0 };
    struct file *module_file;
    size_t file_pages;
    void *module_data;

    static int module_idx = 0;
    ++module_idx;

    if (value_is_object(module_value)) {
        cfg_get_string(cfg, module_value, SV("name"), &module_name);
        CFG_MANDATORY_GET(string, cfg, module_value, SV("path"), &str_path);
    } else {
        str_path = module_value->as_string;
    }

    if (sv_empty(module_name))
        snprintf(attrs->name, sizeof(attrs->name), "unnamed_module%d", module_idx);

    if (!parse_path(str_path, &path))
        oops("invalid module path %pSV", &str_path);

    fse = fs_by_full_path(&path);
    if (!fse)
        oops("invalid module path %pSV", &str_path);

    module_file = fse->fs->open(fse->fs, path.path_within_partition);
    if (!module_file)
        oops("invalid module path %pSV", &str_path);

    file_pages = CEILING_DIVIDE(module_file->size, PAGE_SIZE);
    module_data = allocate_critical_pages_with_type(file_pages, ULTRA_MEMORY_TYPE_MODULE);

    if (!module_file->read(module_file, module_data, 0, module_file->size))
        oops("failed to read module file");

    *attrs = (struct ultra_module_info_attribute) {
        .header = { ULTRA_ATTRIBUTE_MODULE_INFO, sizeof(struct ultra_module_info_attribute) },
        .physical_address = (ptr_t)module_data,
        .length = module_file->size
    };

    fse->fs->close(fse->fs, module_file);
}

struct kernel_info {
    struct binary_options bin_opts;
    struct binary_info bin_info;
};

void load_kernel(struct config *cfg, struct loadable_entry *entry, struct kernel_info *info)
{
    const struct fs_entry *fse;
    struct file *f;
    void *file_data;
    u8 bitness;
    struct load_result res = { 0 };

    get_binary_options(cfg, entry, &info->bin_opts);
    fse = fs_by_full_path(&info->bin_opts.path);

    f = fse->fs->open(fse->fs, info->bin_opts.path.path_within_partition);
    if (!f)
        oops("failed to open %pSV", &info->bin_opts.path.path_within_partition);

    file_data = allocate_critical_bytes(f->size);

    if (!f->read(f, file_data, 0, f->size))
        oops("failed to read file");

    bitness = elf_bitness(file_data, f->size);

    if (!bitness || (bitness != 32 && bitness != 64))
        oops("invalid ELF bitness");

    if (info->bin_opts.allocate_anywhere && bitness != 64)
        oops("allocate-anywhere is only allowed for 64 bit kernels");

    if (bitness == 64 && !cpu_supports_long_mode())
        oops("attempted to load a 64 bit kernel on a CPU without long mode support");

    if (!elf_load(file_data, f->size, bitness == 64, info->bin_opts.allocate_anywhere,
                  ULTRA_MEMORY_TYPE_KERNEL_BINARY, &res))
        oops("failed to load kernel binary: %s", res.error_msg);

    info->bin_info = res.info;
}

enum video_mode_constraint {
    VIDEO_MODE_CONSTRAINT_EXACTLY,
    VIDEO_MODE_CONSTRAINT_AT_LEAST,
};

struct requested_video_mode {
    u32 width, height, bpp;
    enum video_mode_constraint constraint;
    bool none;
};
#define VM_EQUALS(l, r) ((l).width == (r).width && (l).height == (r).height && (l).bpp == (r).bpp)
#define VM_GREATER_OR_EQUAL(l, r) ((l).width >= (r).width && (l).height >= (r).height && (l).bpp >= (r).bpp)
#define VM_LESS_OR_EQUAL(l, r) ((l).width <= (r).width && (l).height <= (r).height)

void video_mode_from_value(struct config *cfg, struct value *val, struct requested_video_mode *mode)
{
    u64 cfg_width, cfg_height, cfg_bpp;
    struct string_view constraint_str;

    if (value_is_null(val)) {
        mode->none = true;
        return;
    }

    if (value_is_string(val)) {
        if (sv_equals(val->as_string, SV("unset"))) {
            mode->none = true;
            return;
        }

        if (!sv_equals(val->as_string, SV("auto")))
            oops("invalid value for \"video-mode\": %pSV", &val->as_string);

        return;
    }

    if (cfg_get_unsigned(cfg, val, SV("width"), &cfg_width))
        mode->width = cfg_width;
    if (cfg_get_unsigned(cfg, val, SV("height"), &cfg_height))
        mode->height = cfg_height;
    if (cfg_get_unsigned(cfg, val, SV("bpp"), &cfg_bpp))
        mode->bpp = cfg_bpp;

    if (cfg_get_string(cfg, val, SV("constraint"), &constraint_str)) {
        if (sv_equals(constraint_str, SV("at-least")))
            mode->constraint = VIDEO_MODE_CONSTRAINT_AT_LEAST;
        else if (sv_equals(constraint_str, SV("exactly")))
            mode->constraint = VIDEO_MODE_CONSTRAINT_EXACTLY;
        else
            oops("invalid video mode constraint %pSV", &constraint_str);
    }
}

#define DEFAULT_WIDTH 1024
#define DEFAULT_HEIGHT 768
#define DEFAULT_BPP 32

bool set_video_mode(struct config *cfg, struct loadable_entry *entry,
                    struct video_services *vs, struct ultra_framebuffer *out_fb)
{
    struct value video_mode_val;
    struct video_mode picked_vm, *mode_list;
    size_t mode_count, mode_idx;
    bool did_pick = false;
    struct resolution native_res = {
        .width = DEFAULT_WIDTH,
        .height = DEFAULT_HEIGHT
    };
    struct requested_video_mode rm = {
        .width = DEFAULT_WIDTH,
        .height = DEFAULT_HEIGHT,
        .bpp = DEFAULT_BPP,
        .constraint = VIDEO_MODE_CONSTRAINT_AT_LEAST
    };
    struct framebuffer fb;

    if (cfg_get_one_of(cfg, entry, SV("video-mode"), VALUE_OBJECT | VALUE_STRING | VALUE_NONE,
                       &video_mode_val)) {
        video_mode_from_value(cfg, &video_mode_val, &rm);
    }

    if (rm.none)
        return false;

    vs->query_resolution(&native_res);
    mode_list = vs->list_modes(&mode_count);

    for (mode_idx = 0; mode_idx < mode_count; ++mode_idx) {
        struct video_mode *m = &mode_list[mode_idx];

        if (rm.constraint == VIDEO_MODE_CONSTRAINT_EXACTLY && VM_EQUALS(*m, rm)) {
            picked_vm = *m;
            did_pick = true;
            break;
        }

        if (VM_GREATER_OR_EQUAL(*m, rm) && VM_LESS_OR_EQUAL(*m, native_res)) {
            picked_vm = *m;
            did_pick = true;
        }
    }

    if (!did_pick) {
        oops("failed to pick a video mode according to constraints (%ux%u %u bpp)",
             rm.width, rm.height, rm.bpp);
    }

    print_info("picked video mode %ux%u @ %u bpp\n", picked_vm.width, picked_vm.height, picked_vm.bpp);

    if (!vs->set_mode(picked_vm.id, &fb))
        oops("failed to set picked video mode");

    BUILD_BUG_ON(sizeof(*out_fb) != sizeof(fb));
    memcpy(out_fb, &fb, sizeof(fb));

    return true;
}

struct attribute_array_spec {
    bool fb_present;
    bool cmdline_present;

    struct ultra_framebuffer fb;

    struct string_view cmdline;
    struct kernel_info kern_info;

    struct ultra_module_info_attribute *modules;
    size_t module_count;

    u64 stack_address;
    ptr_t acpi_rsdp_address;
};

struct handover_info {
    size_t memory_map_handover_key;
    u64 attribute_array_address;
};
#define LOAD_NAME_STRING "HyperLoader v0.1"

static void ultra_memory_map_entry_convert(struct memory_map_entry *entry, void *buf)
{
    struct ultra_memory_map_entry *ue = buf;

    ue->physical_address = entry->physical_address;
    ue->size_in_bytes = entry->size_in_bytes;

    // Direct mapping
    if (entry->type <= ULTRA_MEMORY_TYPE_NVS || (entry->type >= ULTRA_MEMORY_TYPE_LOADER_RECLAIMABLE)) {
        ue->type = entry->type;
    } else {
        ue->type = ULTRA_MEMORY_TYPE_RESERVED;
    }
}

static void create_kernel_info_attribute(struct ultra_kernel_info_attribute *attr, const struct kernel_info *ki)
{
    struct string_view path_str = ki->bin_opts.path.path_within_partition;

    attr->header = (struct ultra_attribute_header) {
        .type = ULTRA_ATTRIBUTE_KERNEL_INFO,
        .size_in_bytes = sizeof(struct ultra_kernel_info_attribute)
    };
    attr->physical_base = ki->bin_info.physical_base;
    attr->virtual_base = ki->bin_info.virtual_base;
    attr->range_length = ki->bin_info.physical_ceiling - ki->bin_info.physical_base;
    attr->partition_type = ki->bin_opts.path.partition_id_type;
    attr->partition_index = ki->bin_opts.path.partition_index;

    BUILD_BUG_ON(sizeof(attr->disk_guid) != sizeof(ki->bin_opts.path.disk_guid));
    memcpy(&attr->disk_guid, &ki->bin_opts.path.disk_guid, sizeof(attr->disk_guid));
    memcpy(&attr->partition_guid, &ki->bin_opts.path.partition_guid, sizeof(attr->partition_guid));

    BUG_ON(path_str.size > (sizeof(attr->path_on_disk) - 1));
    memcpy(attr->path_on_disk, path_str.text, path_str.size);
    attr->path_on_disk[path_str.size] = '\0';
}

void build_attribute_array(const struct attribute_array_spec *spec, enum service_provider sp,
                           struct memory_services *ms, struct handover_info *hi)
{
    struct string_view loader_name_str = SV(LOAD_NAME_STRING);
    u32 cmdline_aligned_length = 0;
    size_t memory_map_reserved_size, bytes_needed = 0;
    void *attr_ptr;
    uint32_t *attr_count;

    if (spec->cmdline_present) {
        cmdline_aligned_length += sizeof(struct ultra_attribute_header);
        cmdline_aligned_length += spec->cmdline.size + 1;
        size_t remainder = cmdline_aligned_length % 8;

        if (remainder)
            cmdline_aligned_length += 8 - remainder;
    }

    bytes_needed += sizeof(uint64_t); // attribute_count
    bytes_needed += sizeof(struct ultra_platform_info_attribute);
    bytes_needed += sizeof(struct ultra_kernel_info_attribute);
    bytes_needed += spec->module_count * sizeof(struct ultra_module_info_attribute);
    bytes_needed += cmdline_aligned_length;
    bytes_needed += spec->fb_present * sizeof(struct ultra_framebuffer_attribute);

    /*
     * Attempt to allocate the storage for attribute array while having enough space for the memory map
     * (which is changed every time we allocate/free more memory)
     */
    for (;;) {
        size_t bytes_for_this_allocation, memory_map_size_new, key = 0;

        // Add 1 to give some leeway for memory map growth after the next allocation
        memory_map_reserved_size = ms->copy_map(NULL, 0, 0, &key, NULL) + 1;
        bytes_for_this_allocation = bytes_needed + memory_map_reserved_size * sizeof(struct ultra_memory_map_entry);
        hi->attribute_array_address = (u32)allocate_critical_bytes(bytes_for_this_allocation);

        // Check if memory map had to grow to store the previous allocation
        memory_map_size_new = ms->copy_map(NULL, 0, 0, &key, NULL);
        if (memory_map_reserved_size >= memory_map_size_new) {
            memzero((void*)(ptr_t)hi->attribute_array_address, bytes_for_this_allocation);
            break;
        }

        free_bytes((void*)(ptr_t)hi->attribute_array_address, bytes_for_this_allocation);
    }

    attr_ptr = (void*)(ptr_t)hi->attribute_array_address;
    attr_ptr += sizeof(uint32_t);
    attr_count = attr_ptr;
    attr_ptr += sizeof(uint32_t);

    *attr_count = 0;

    *(struct ultra_platform_info_attribute*)attr_ptr = (struct ultra_platform_info_attribute) {
        .header = { ULTRA_ATTRIBUTE_PLATFORM_INFO, sizeof(struct ultra_platform_info_attribute) },
        .platform_type = sp == SERVICE_PROVIDER_BIOS ? ULTRA_PLATFORM_BIOS : ULTRA_PLATFORM_UEFI,
        .loader_major = 0,
        .loader_minor = 1,
        .acpi_rsdp_address = spec->acpi_rsdp_address
    };
    memcpy(((struct ultra_platform_info_attribute*)attr_ptr)->loader_name, loader_name_str.text,
          loader_name_str.size + 1);
    attr_ptr += sizeof(struct ultra_platform_info_attribute);
    *attr_count += 1;

    create_kernel_info_attribute(attr_ptr, &spec->kern_info);
    attr_ptr += sizeof(struct ultra_kernel_info_attribute);
    *attr_count += 1;

    if (spec->module_count) {
        size_t bytes_for_modules = spec->module_count * sizeof(struct ultra_module_info_attribute);
        memcpy(attr_ptr, spec->modules, bytes_for_modules);
        attr_ptr += bytes_for_modules;
        *attr_count += spec->module_count;
    }

    if (spec->cmdline_present) {
        *(struct ultra_command_line_attribute*)attr_ptr = (struct ultra_command_line_attribute) {
            .header = { ULTRA_ATTRIBUTE_COMMAND_LINE, cmdline_aligned_length },
        };

        // Copy the cmdline string & null terminate
        memcpy(attr_ptr + sizeof(struct ultra_command_line_attribute), spec->cmdline.text, spec->cmdline.size);
        *((char*)attr_ptr + sizeof(struct ultra_attribute_header) + spec->cmdline.size) = '\0';

        attr_ptr += cmdline_aligned_length;
        *attr_count += 1;
    }

    if (spec->fb_present) {
        *(struct ultra_framebuffer_attribute*)attr_ptr = (struct ultra_framebuffer_attribute) {
            .header = { ULTRA_ATTRIBUTE_FRAMEBUFFER_INFO, sizeof(struct ultra_framebuffer_attribute) },
            .fb = spec->fb
        };

        attr_ptr += sizeof(struct ultra_framebuffer_attribute);
        *attr_count += 1;
    }

    *(struct ultra_memory_map_attribute*)attr_ptr = (struct ultra_memory_map_attribute) {
        .header = {
            .type = ULTRA_ATTRIBUTE_MEMORY_MAP,
            .size_in_bytes = memory_map_reserved_size * sizeof(struct ultra_memory_map_entry)
                             + sizeof(struct ultra_memory_map_attribute)
        }
    };
    attr_ptr += sizeof(struct ultra_attribute_header);
    *attr_count += 1;

    ms->copy_map(attr_ptr, memory_map_reserved_size, sizeof(struct ultra_memory_map_entry),
                 &hi->memory_map_handover_key, ultra_memory_map_entry_convert);
    attr_ptr += memory_map_reserved_size * sizeof(struct ultra_memory_map_entry);
}

u64 build_page_table(struct binary_info *bi)
{
    struct page_table pt;

    if (bi->bitness != 64)
        return 0;

    pt.root = (u64*)allocate_critical_pages(1);
    pt.levels = 4;
    memzero(pt.root, PAGE_SIZE);

    // identity map bottom 4 gigabytes
    map_critical_huge_pages(&pt, 0x0000000000000000, 0x0000000000000000,
                            (4ull * GB) / HUGE_PAGE_SIZE);

    // direct map higher half
    map_critical_huge_pages(&pt, DIRECT_MAP_BASE , 0x0000000000000000,
                            (4ull * GB) / HUGE_PAGE_SIZE);

    /*
     * If kernel had allocate-anywhere set to on, map virtual base to physical base,
     * otherwise simply direct map fist 2 gigabytes of physical.
     */
    if (!bi->kernel_range_is_direct_map) {
        size_t pages = bi->physical_ceiling - bi->physical_base;
        pages = CEILING_DIVIDE(pages, PAGE_SIZE);
        map_critical_pages(&pt, bi->virtual_base, bi->physical_base, pages);
    } else {
        map_critical_huge_pages(&pt, HIGHER_HALF_BASE, 0x0000000000000000,
                                (2ull * GB) / HUGE_PAGE_SIZE);
    }

    return (ptr_t)pt.root;
}

u64 pick_stack(struct config *cfg, struct loadable_entry *le)
{
    struct value val;
    u64 address = 0;
    size_t size = 16 * KB;
    bool has_val;

    has_val = cfg_get_one_of(cfg, le, SV("stack"), VALUE_STRING | VALUE_OBJECT, &val);

    if (has_val && value_is_object(&val)) {
        struct value alloc_at_val, size_val;
        bool has_alloc_at, has_size;

        has_alloc_at = cfg_get_one_of(cfg, le, SV("allocate-at"), VALUE_STRING | VALUE_UNSIGNED, &alloc_at_val);
        has_size = cfg_get_one_of(cfg, le, SV("size"), VALUE_STRING | VALUE_UNSIGNED, &size_val);

        if (has_alloc_at && value_is_string(&alloc_at_val)) {
            if (!sv_equals(alloc_at_val.as_string, SV("anywhere")))
                oops("invalid value for \"allocate-at\": %pSV", &alloc_at_val.as_string);
        } else if (has_alloc_at) { // unsigned
            address = alloc_at_val.as_unsigned;
        }

        if (has_size && value_is_string(&size_val)) {
            if (!sv_equals(size_val.as_string, SV("auto")))
                oops("invalid value for \"size\": %pSV", &size_val.as_string);
        } else if (has_size) { // unsigned
            size = size_val.as_unsigned;
        }
    } else if (has_val) { // string
        if (!sv_equals(val.as_string, SV("auto")))
            oops("invalid value for \"stack\": %pSV", &val.as_string);
    }

    size_t pages = CEILING_DIVIDE(size, PAGE_SIZE);

    if (address)
        allocate_critical_pages_with_type_at(address, pages, ULTRA_MEMORY_TYPE_KERNEL_STACK);
    else
        address = (ptr_t)allocate_critical_pages_with_type(pages, ULTRA_MEMORY_TYPE_KERNEL_STACK);

    address += pages * PAGE_SIZE;
    return address;
}

#define MODULES_PER_PAGE (PAGE_SIZE / sizeof(struct ultra_module_info_attribute))

void ultra_protocol_load(struct config *cfg, struct loadable_entry *le, struct services *sv)
{
    struct attribute_array_spec spec = { 0 };
    size_t modules_capacity = MODULES_PER_PAGE;
    spec.modules = allocate_critical_pages(1);
    struct value module_value;
    struct handover_info hi;
    u64 pt;
    bool handover_res;
    bool is_higher_half_kernel;

    load_kernel(cfg, le, &spec.kern_info);
    is_higher_half_kernel = spec.kern_info.bin_info.entrypoint_address >= HIGHER_HALF_BASE;

    spec.cmdline_present = cfg_get_string(cfg, le, SV("cmdline"), &spec.cmdline);

    if (cfg_get_first_one_of(cfg, le, SV("module"), VALUE_STRING | VALUE_OBJECT, &module_value)) {
        do {
            if (++spec.module_count == modules_capacity) {
                void *new_modules = allocate_critical_pages(modules_capacity + MODULES_PER_PAGE);
                memcpy(new_modules, spec.modules, modules_capacity);
                free_pages(spec.modules, (modules_capacity * sizeof(struct ultra_module_info_attribute)) / PAGE_SIZE);
                spec.modules = new_modules;
                modules_capacity += MODULES_PER_PAGE;
            }

            module_load(cfg, &module_value, &spec.modules[spec.module_count - 1]);
        } while (cfg_get_next_one_of(cfg, VALUE_STRING | VALUE_OBJECT, &module_value, true));
    }

    pt = build_page_table(&spec.kern_info.bin_info);
    spec.stack_address = pick_stack(cfg, le);
    spec.acpi_rsdp_address = sv->get_rsdp();

    /*
    * Attempt to set video mode last, as we're not going to be able to use
    * legacy tty logging after that.
    */
    spec.fb_present = set_video_mode(cfg, le, sv->vs, &spec.fb);
    if (is_higher_half_kernel)
        spec.fb.physical_address += DIRECT_MAP_BASE;

    /*
     * We cannot allocate any memory after this call, as memory map is now
     * saved inside the attribute array.
     */
    build_attribute_array(&spec, sv->provider, sv->ms, &hi);
    handover_res = sv->ms->handover(hi.memory_map_handover_key);
    BUG_ON(!handover_res);

    // Relocate pointers to higher half for convenience
    if (is_higher_half_kernel) {
        spec.stack_address += DIRECT_MAP_BASE;
        hi.attribute_array_address += DIRECT_MAP_BASE;
    }

    print_info("jumping to kernel: entry 0x%016llX, stack at 0x%016llX, boot context at 0x%016llX\n",
               spec.kern_info.bin_info.entrypoint_address, spec.stack_address,
               hi.attribute_array_address);

    if (spec.kern_info.bin_info.bitness == 32)
        kernel_handover32(spec.kern_info.bin_info.entrypoint_address, spec.stack_address,
                          (u32)hi.attribute_array_address, ULTRA_MAGIC);

    kernel_handover64(spec.kern_info.bin_info.entrypoint_address, spec.stack_address, pt,
                      hi.attribute_array_address, ULTRA_MAGIC);
}
