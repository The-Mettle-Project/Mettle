#include "linker/link_object.h"
#include "linker/coff_reader.h"
#include "linker/elf_reader.h"
#include "linker/linker_common.h"
#include "../common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int link_object_read_file(const char *filename, unsigned char **data_out, size_t *size_out, char **error_message_out) {
    FILE *file = NULL;
    long file_size = 0;
    size_t bytes_read = 0;
    unsigned char *data = NULL;

    if (!filename || !data_out || !size_out) {
        mettle_set_error(error_message_out, "Invalid arguments while reading object file");
        return 0;
    }

    file = fopen(filename, "rb");
    if (!file) {
        mettle_set_error(error_message_out, "Failed to open '%s': '%s'", filename, strerror(errno));
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        mettle_set_error(error_message_out, "Failed to seek out end of '%s'", filename);
        fclose(file);
        return 0;
    }
    file_size = ftell(file);
    if (file_size < 0) {
        mettle_set_error(error_message_out, "Failed to determine size of '%s'", filename);
        fclose(file);
        return 0;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        mettle_set_error(error_message_out, "Failed to rewind '%s'", filename);
        fclose(file);
        return 0;
    }

    data = malloc((size_t)file_size ? (size_t)file_size : 1u);
    if (!data) {
        mettle_set_error(error_message_out, "Out of memory while loading '%s'", filename);
        fclose(file);
        return 0;
    }

    bytes_read = fread(data, 1, (size_t)file_size, file);
    fclose(file);
    if (bytes_read != (size_t)file_size) {
        free(data);
        mettle_set_error(error_message_out, "Failed to read '%s'", filename);
        return 0;
    }

    *data_out = data;
    *size_out = (size_t)file_size;
    return 1;
}

int link_object_range_ok(size_t file_size, uint64_t offset, uint64_t length) {
    if (offset > file_size) {
        return 0;
    }
    if (length > (uint64_t)file_size - offset) {
        return 0;
    }
    return 1;
}

void link_object_destroy(LinkObject *object) {
    size_t i = 0;

    if (!object) {
        return;
    }
    if (object->sections) {
        for (i = 0; i < object->section_count; i++) {
            free(object->sections[i].name);
            free(object->sections[i].raw_data);
            free(object->sections[i].relocations);
        }
        free(object->sections);
    }
    if (object->symbols) {
        for (i = 0; i < object->symbol_count; i++) {
            free(object->symbols[i].name);
        }
        free(object->symbols);
    }
    free(object->string_table);
    free(object);
}

const LinkSection *link_object_find_section_by_kind(const LinkObject *object, LinkSectionKind kind) {
    size_t i = 0;

    if (!object || !object->sections) {
        return NULL;
    }
    for (i = 0; i < object->section_count; i++) {
        if (object->sections[i].kind == kind) {
            return &object->sections[i];
        }
    }
    return NULL;
}

const LinkSymbol *link_object_find_symbol(const LinkObject *object, const char *name) {
    size_t i = 0;

    if (!object || !object->symbols || !name) {
        return NULL;
    }
    for (i = 0; i < object->symbol_count; i++) {
        if (object->symbols[i].name && strcmp(object->symbols[i].name, name) == 0) {
            return &object->symbols[i];
        }
    }
    return NULL;
}

const char *link_section_kind_name(LinkSectionKind kind) {
    switch (kind) {
        case LINK_SECTION_KIND_TEXT: return ".text";
        case LINK_SECTION_KIND_RDATA: return ".rdata";
        case LINK_SECTION_KIND_DATA: return ".data";
        case LINK_SECTION_KIND_BSS: return ".bss";
        case LINK_SECTION_KIND_TLS: return ".tls";
        case LINK_SECTION_KIND_PDATA: return ".pdata";
        case LINK_SECTION_KIND_XDATA: return ".xdata";
        case LINK_SECTION_KIND_UNKNOWN: break;
    }
    return "<unknown>";
}

const char *link_reloc_kind_name(LinkRelocKind kind) {
    switch (kind) {
        case LINK_RELOC_ABS64: return "ABS64";
        case LINK_RELOC_PC32: return "PC32";
        case LINK_RELOC_ABS32: return "ABS32";
        case LINK_RELOC_IMAGE_REL32: return "REL32";
        case LINK_RELOC_SECREL32: return "SECREL32";
        case LINK_RELOC_TPOFF32: return "TPOFF32";
        case LINK_RELOC_GOTPCREL32: return "GOTPCREL32";
        case LINK_RELOC_NONE: default: return "NONE";
    }
}

static LinkSectionKind link_section_kind_from_coff(CoffSectionKind kind) {
    switch (kind) {
        case COFF_SECTION_KIND_TEXT: return LINK_SECTION_KIND_TEXT;
        case COFF_SECTION_KIND_RDATA: return LINK_SECTION_KIND_RDATA;
        case COFF_SECTION_KIND_DATA: return LINK_SECTION_KIND_DATA;
        case COFF_SECTION_KIND_BSS: return LINK_SECTION_KIND_BSS;
        case COFF_SECTION_KIND_PDATA: return LINK_SECTION_KIND_PDATA;
        case COFF_SECTION_KIND_XDATA: return LINK_SECTION_KIND_XDATA;
        case COFF_SECTION_KIND_UNKNOWN: break;
    }
    return LINK_SECTION_KIND_UNKNOWN;
}

static LinkRelocKind link_reloc_kind_from_coff(uint16_t type) {
    switch (type) {
        case COFF_RELOC_AMD64_ADDR64: return LINK_RELOC_ABS64;
        case COFF_RELOC_AMD64_ADDR32: return LINK_RELOC_ABS32;
        case COFF_RELOC_AMD64_ADDR32NB: return LINK_RELOC_IMAGE_REL32;
        case COFF_RELOC_AMD64_REL32: return LINK_RELOC_PC32;
        case COFF_RELOC_AMD64_SECREL: return LINK_RELOC_SECREL32;
        default: return LINK_RELOC_NONE;
    }
}

static uint64_t link_alignment_from_coff(uint32_t characteristics) {
    uint32_t field = (characteristics & 0x00F00000u) >> 20;

    return field ? (uint64_t)1u << (field - 1u) : 1u;
}

static int link_object_adopt_coff_sections(const CoffObject *coff, LinkObject *object, char **error_message_out) {
    size_t i = 0;
    size_t r = 0;

    object->sections = calloc(coff->section_count ? coff->section_count : 1u, sizeof(LinkSection));
    if (!object->sections) {
        mettle_set_error(error_message_out, "Out of memory while converting COFF sections");
        return 0;
    }
    object->section_count = coff->section_count;

    for (i = 0; i < object->section_count; i++) {
        const CoffSection *source = &coff->sections[i];
        LinkSection *section = &object->sections[i];

        section->name = source->name ? mettle_strdup(source->name) : NULL;
        section->kind = link_section_kind_from_coff(source->kind);
        section->alignment = link_alignment_from_coff(source->characteristics);
        section->virtual_size = source->size_of_raw_data;
        section->is_metadata = source->kind == COFF_SECTION_KIND_UNKNOWN;

        if (source->raw_data && source->size_of_raw_data > 0u) {
            section->raw_data = malloc(source->size_of_raw_data);
            if (!section->raw_data) {
                mettle_set_error(error_message_out, "Out of memory while converting COFF section data");
                return 0;
            }
            memcpy(section->raw_data, source->raw_data, source->size_of_raw_data);
            section->size_of_raw_data = source->size_of_raw_data;
        }

        if (source->relocation_count == 0u) {
            continue;
        }
        section->relocations = calloc(source->relocation_count, sizeof(LinkReloc));
        if (!section->relocations) {
            mettle_set_error(error_message_out, "Out of memory while converting COFF relocations");
            return 0;
        }
        section->relocation_count = source->relocation_count;
        for (r = 0; r < source->relocation_count; r++) {
            const CoffRelocation *source_reloc = &source->relocations[r];
            LinkReloc *reloc = &section->relocations[r];

            reloc->offset = source_reloc->virtual_address;
            reloc->symbol_index = source_reloc->symbol_table_index;
            reloc->kind = link_reloc_kind_from_coff(source_reloc->type);
            reloc->format_type = source_reloc->type;
        }
    }
    return 1;
}

static int link_object_adopt_coff_symbols(const CoffObject *coff, LinkObject *object, char **error_message_out) {
    size_t i = 0;

    object->symbols = calloc(coff->symbol_count ? coff->symbol_count : 1u, sizeof(LinkSymbol));
    if (!object->symbols) {
        mettle_set_error(error_message_out, "Out of memory while converting COFF symbols");
        return 0;
    }
    object->symbol_count = coff->symbol_count;

    for (i = 0; i < object->symbol_count; i++) {
        const CoffSymbol *source = &coff->symbols[i];
        LinkSymbol *symbol = &object->symbols[i];

        symbol->name = source->name ? mettle_strdup(source->name) : NULL;
        symbol->value = source->value;
        symbol->is_auxiliary = source->is_auxiliary;
        symbol->aux_section_length =
            source->has_auxiliary_record ? source->aux_section_length : 0u;
        symbol->section_index = LINK_SECTION_INDEX_UNDEFINED;

        if (source->is_auxiliary) {
            continue;
        }
        symbol->is_external = source->storage_class == COFF_STORAGE_CLASS_EXTERNAL;
        symbol->is_weak = source->storage_class == COFF_STORAGE_CLASS_WEAK_EXTERNAL;

        if (source->section_number > 0) {
            if ((size_t)source->section_number > object->section_count) {
                mettle_set_error(error_message_out, "COFF symbol '%s' names section %d outside the section table", symbol->name ? symbol->name : "<unnamed>", (int)source->section_number);
                return 0;
            }
            symbol->section_index = (int64_t)source->section_number - 1;
            symbol->is_defined = 1;
        } else if (source->section_number == COFF_SECTION_NUMBER_ABSOLUTE) {
            symbol->section_index = LINK_SECTION_INDEX_ABSOLUTE;
            symbol->is_defined = 1;
        } else if (source->section_number == 0 && source->value != 0u) {
            symbol->section_index = LINK_SECTION_INDEX_COMMON;
            symbol->is_common = 1;
            symbol->is_defined = 1;
        }
    }
    return 1;
}

static int link_object_from_coff(const char *filename, LinkObject **object_out, char **error_message_out) {
    CoffObject *coff = NULL;
    LinkObject *object = NULL;

    if (!coff_object_read(filename, &coff, error_message_out)) {
        return 0;
    }
    object = calloc(1, sizeof(LinkObject));
    if (!object) {
        mettle_set_error(error_message_out, "Out of memory while creating COFF object");
        coff_object_destroy(coff);
        return 0;
    }
    object->format = LINK_FORMAT_COFF;

    if (!link_object_adopt_coff_sections(coff, object, error_message_out) ||
        !link_object_adopt_coff_symbols(coff, object, error_message_out)) {
        coff_object_destroy(coff);
        link_object_destroy(object);
        return 0;
    }
    coff_object_destroy(coff);
    *object_out = object;
    return 1;
}

static int link_object_looks_like_elf(const char *filename) {
    unsigned char magic[4] = {0};
    FILE *file = fopen(filename, "rb");
    size_t read = 0;

    if (!file) {
        return 0;
    }
    read = fread(magic, 1, sizeof(magic), file);
    fclose(file);
    return read == sizeof(magic) && magic[0] == 0x7Fu && magic[1] == 'E' &&
           magic[2] == 'L' && magic[3] == 'F';
}

int link_object_read(const char *filename, LinkObject **object_out, char **error_message_out) {
    if (object_out) {
        *object_out = NULL;
    }
    if (error_message_out) {
        free(*error_message_out);
        *error_message_out = NULL;
    }
    if (!filename || !object_out) {
        mettle_set_error(error_message_out, "Invalid arguments while reading object file");
        return 0;
    }
    if (link_object_looks_like_elf(filename)) {
        return elf_object_read(filename, object_out, error_message_out);
    }
    return link_object_from_coff(filename, object_out, error_message_out);
}
