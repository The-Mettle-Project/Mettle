#include "debug_info.h"
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_SYMBOL_CAPACITY 64
#define INITIAL_MAPPING_CAPACITY 256
#define INITIAL_RUNTIME_FUNCTION_CAPACITY 64
#define INITIAL_RUNTIME_LOCATION_CAPACITY 256
#define INITIAL_RUNTIME_TRAP_SITE_CAPACITY 64

DebugInfo* debug_info_create(const char* source_filename, const char* assembly_filename) {
    DebugInfo* debug_info = malloc(sizeof(DebugInfo));
    if (!debug_info) return NULL;
    
    debug_info->symbols = malloc(sizeof(DebugSymbol) * INITIAL_SYMBOL_CAPACITY);
    if (!debug_info->symbols) {
        free(debug_info);
        return NULL;
    }
    
    debug_info->line_mappings = malloc(sizeof(SourceLineMapping) * INITIAL_MAPPING_CAPACITY);
    if (!debug_info->line_mappings) {
        free(debug_info->symbols);
        free(debug_info);
        return NULL;
    }
    
    debug_info->symbol_count = 0;
    debug_info->symbol_capacity = INITIAL_SYMBOL_CAPACITY;
    debug_info->mapping_count = 0;
    debug_info->mapping_capacity = INITIAL_MAPPING_CAPACITY;
    debug_info->runtime_functions =
        malloc(sizeof(RuntimeFunctionMapping) * INITIAL_RUNTIME_FUNCTION_CAPACITY);
    if (!debug_info->runtime_functions) {
        free(debug_info->line_mappings);
        free(debug_info->symbols);
        free(debug_info);
        return NULL;
    }

    debug_info->runtime_locations =
        malloc(sizeof(RuntimeLocationMapping) * INITIAL_RUNTIME_LOCATION_CAPACITY);
    if (!debug_info->runtime_locations) {
        free(debug_info->runtime_functions);
        free(debug_info->line_mappings);
        free(debug_info->symbols);
        free(debug_info);
        return NULL;
    }

    debug_info->runtime_function_count = 0;
    debug_info->runtime_function_capacity = INITIAL_RUNTIME_FUNCTION_CAPACITY;
    debug_info->runtime_location_count = 0;
    debug_info->runtime_location_capacity = INITIAL_RUNTIME_LOCATION_CAPACITY;

    debug_info->runtime_trap_sites =
        malloc(sizeof(RuntimeTrapSiteMapping) * INITIAL_RUNTIME_TRAP_SITE_CAPACITY);
    if (!debug_info->runtime_trap_sites) {
        free(debug_info->runtime_locations);
        free(debug_info->runtime_functions);
        free(debug_info->line_mappings);
        free(debug_info->symbols);
        free(debug_info);
        return NULL;
    }

    debug_info->runtime_trap_site_count = 0;
    debug_info->runtime_trap_site_capacity = INITIAL_RUNTIME_TRAP_SITE_CAPACITY;
    
    debug_info->source_filename = mettle_strdup(source_filename);
    debug_info->assembly_filename = mettle_strdup(assembly_filename);
    
    return debug_info;
}

void debug_info_destroy(DebugInfo* debug_info) {
    if (!debug_info) return;
    
    for (size_t i = 0; i < debug_info->symbol_count; i++) {
        free(debug_info->symbols[i].name);
        free(debug_info->symbols[i].type_name);
        free(debug_info->symbols[i].register_name);
    }
    free(debug_info->symbols);
    
    for (size_t i = 0; i < debug_info->mapping_count; i++) {
        free(debug_info->line_mappings[i].filename);
    }
    free(debug_info->line_mappings);

    for (size_t i = 0; i < debug_info->runtime_function_count; i++) {
        free(debug_info->runtime_functions[i].function_name);
        free(debug_info->runtime_functions[i].start_label);
        free(debug_info->runtime_functions[i].end_label);
        free(debug_info->runtime_functions[i].filename);
    }
    free(debug_info->runtime_functions);

    for (size_t i = 0; i < debug_info->runtime_location_count; i++) {
        free(debug_info->runtime_locations[i].function_name);
        free(debug_info->runtime_locations[i].address_label);
        free(debug_info->runtime_locations[i].filename);
    }
    free(debug_info->runtime_locations);

    for (size_t i = 0; i < debug_info->runtime_trap_site_count; i++) {
        free(debug_info->runtime_trap_sites[i].address_label);
        free(debug_info->runtime_trap_sites[i].function_name);
        free(debug_info->runtime_trap_sites[i].filename);
        free(debug_info->runtime_trap_sites[i].source_line);
        free(debug_info->runtime_trap_sites[i].message_template);
        free(debug_info->runtime_trap_sites[i].static_context);
    }
    free(debug_info->runtime_trap_sites);
    
    free(debug_info->source_filename);
    free(debug_info->assembly_filename);
    free(debug_info);
}

static int debug_info_expand_symbols(DebugInfo* debug_info) {
    if (debug_info->symbol_count >= debug_info->symbol_capacity) {
        size_t new_capacity = debug_info->symbol_capacity * 2;
        DebugSymbol* new_symbols = realloc(debug_info->symbols, 
                                          sizeof(DebugSymbol) * new_capacity);
        if (!new_symbols) return 0;
        
        debug_info->symbols = new_symbols;
        debug_info->symbol_capacity = new_capacity;
    }
    return 1;
}

static int debug_info_expand_mappings(DebugInfo* debug_info) {
    if (debug_info->mapping_count >= debug_info->mapping_capacity) {
        size_t new_capacity = debug_info->mapping_capacity * 2;
        SourceLineMapping* new_mappings = realloc(debug_info->line_mappings,
                                                 sizeof(SourceLineMapping) * new_capacity);
        if (!new_mappings) return 0;
        
        debug_info->line_mappings = new_mappings;
        debug_info->mapping_capacity = new_capacity;
    }
    return 1;
}

static int debug_info_expand_runtime_functions(DebugInfo* debug_info) {
    if (debug_info->runtime_function_count >= debug_info->runtime_function_capacity) {
        size_t new_capacity = debug_info->runtime_function_capacity * 2;
        RuntimeFunctionMapping* new_items = realloc(
            debug_info->runtime_functions,
            sizeof(RuntimeFunctionMapping) * new_capacity);
        if (!new_items) return 0;

        debug_info->runtime_functions = new_items;
        debug_info->runtime_function_capacity = new_capacity;
    }
    return 1;
}

static int debug_info_expand_runtime_locations(DebugInfo* debug_info) {
    if (debug_info->runtime_location_count >= debug_info->runtime_location_capacity) {
        size_t new_capacity = debug_info->runtime_location_capacity * 2;
        RuntimeLocationMapping* new_items = realloc(
            debug_info->runtime_locations,
            sizeof(RuntimeLocationMapping) * new_capacity);
        if (!new_items) return 0;

        debug_info->runtime_locations = new_items;
        debug_info->runtime_location_capacity = new_capacity;
    }
    return 1;
}

static int debug_info_expand_runtime_trap_sites(DebugInfo* debug_info) {
    if (debug_info->runtime_trap_site_count >=
        debug_info->runtime_trap_site_capacity) {
        size_t new_capacity = debug_info->runtime_trap_site_capacity * 2;
        RuntimeTrapSiteMapping* new_items = realloc(
            debug_info->runtime_trap_sites,
            sizeof(RuntimeTrapSiteMapping) * new_capacity);
        if (!new_items) return 0;

        debug_info->runtime_trap_sites = new_items;
        debug_info->runtime_trap_site_capacity = new_capacity;
    }
    return 1;
}

void debug_info_add_symbol(DebugInfo* debug_info, const char* name, DebugSymbolType type,
                          const char* type_name, size_t line, size_t column) {
    if (!debug_info || !name) return;
    
    if (!debug_info_expand_symbols(debug_info)) return;
    
    DebugSymbol* symbol = &debug_info->symbols[debug_info->symbol_count];
    symbol->name = mettle_strdup(name);
    if (!symbol->name) return;
    symbol->type = type;
    symbol->type_name = mettle_strdup(type_name);
    if (!symbol->type_name) {
        free(symbol->name);
        symbol->name = NULL;
        return;
    }
    symbol->line = line;
    symbol->column = column;
    symbol->address = 0;
    symbol->size = 0;
    symbol->is_register = 0;
    symbol->register_name = NULL;
    symbol->stack_offset = 0;
    
    debug_info->symbol_count++;
}

void debug_info_set_symbol_stack_offset(DebugInfo* debug_info, const char* name, int stack_offset) {
    DebugSymbol* symbol = debug_info_find_symbol(debug_info, name);
    if (symbol) {
        symbol->is_register = 0;
        symbol->stack_offset = stack_offset;
    }
}

DebugSymbol* debug_info_find_symbol(DebugInfo* debug_info, const char* name) {
    if (!debug_info || !name) return NULL;
    
    for (size_t i = 0; i < debug_info->symbol_count; i++) {
        if (debug_info->symbols[i].name && strcmp(debug_info->symbols[i].name, name) == 0) {
            return &debug_info->symbols[i];
        }
    }
    return NULL;
}

void debug_info_add_line_mapping(DebugInfo* debug_info, size_t source_line, size_t source_column,
                                size_t assembly_line, const char* filename) {
    if (!debug_info) return;
    
    if (!debug_info_expand_mappings(debug_info)) return;
    
    SourceLineMapping* mapping = &debug_info->line_mappings[debug_info->mapping_count];
    mapping->source_line = source_line;
    mapping->source_column = source_column;
    mapping->assembly_line = assembly_line;
    mapping->filename = mettle_strdup(filename);
    
    debug_info->mapping_count++;
}

void debug_info_add_runtime_function_mapping(DebugInfo* debug_info,
                                             const char* function_name,
                                             const char* start_label,
                                             const char* end_label,
                                             const char* filename,
                                             size_t line, size_t column) {
    if (!debug_info || !function_name || !start_label || !end_label) return;

    if (!debug_info_expand_runtime_functions(debug_info)) return;

    RuntimeFunctionMapping* mapping =
        &debug_info->runtime_functions[debug_info->runtime_function_count];
    mapping->function_name = mettle_strdup(function_name);
    mapping->start_label = mettle_strdup(start_label);
    mapping->end_label = mettle_strdup(end_label);
    mapping->filename = mettle_strdup(filename);
    mapping->line = line;
    mapping->column = column;
    debug_info->runtime_function_count++;
}

void debug_info_add_runtime_location_mapping(DebugInfo* debug_info,
                                             const char* function_name,
                                             const char* address_label,
                                             const char* filename,
                                             size_t line, size_t column) {
    if (!debug_info || !function_name || !address_label) return;

    if (!debug_info_expand_runtime_locations(debug_info)) return;

    RuntimeLocationMapping* mapping =
        &debug_info->runtime_locations[debug_info->runtime_location_count];
    mapping->function_name = mettle_strdup(function_name);
    mapping->address_label = mettle_strdup(address_label);
    mapping->filename = mettle_strdup(filename);
    mapping->line = line;
    mapping->column = column;
    debug_info->runtime_location_count++;
}

char* debug_info_read_source_line(const char* filename, size_t line_number) {
    char buffer[4096];
    size_t current_line = 1;
    FILE* file = NULL;
    char* result = NULL;

    if (!filename || line_number == 0) {
        return NULL;
    }

    file = fopen(filename, "rb");
    if (!file) {
        return NULL;
    }

    while (fgets(buffer, (int)sizeof(buffer), file)) {
        if (current_line == line_number) {
            size_t length = strlen(buffer);
            while (length > 0 &&
                   (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
                buffer[--length] = '\0';
            }
            result = mettle_strdup(buffer);
            break;
        }
        current_line++;
    }

    fclose(file);
    return result;
}

void debug_info_add_runtime_trap_site_mapping(
    DebugInfo* debug_info, const char* address_label, uint32_t kind,
    const char* function_name, const char* filename, size_t line,
    size_t column, const char* source_line, const char* message_template,
    const char* static_context) {
    RuntimeTrapSiteMapping* mapping = NULL;

    if (!debug_info || !address_label || !function_name) {
        return;
    }

    if (!debug_info_expand_runtime_trap_sites(debug_info)) {
        return;
    }

    mapping = &debug_info->runtime_trap_sites[debug_info->runtime_trap_site_count];
    mapping->address_label = mettle_strdup(address_label);
    mapping->kind = kind;
    mapping->function_name = mettle_strdup(function_name);
    mapping->filename = mettle_strdup(filename ? filename : "");
    mapping->line = line;
    mapping->column = column;
    mapping->source_line = source_line ? mettle_strdup(source_line) : NULL;
    mapping->message_template =
        message_template ? mettle_strdup(message_template) : NULL;
    mapping->static_context =
        static_context ? mettle_strdup(static_context) : NULL;
    if (!mapping->address_label || !mapping->function_name || !mapping->filename) {
        free(mapping->address_label);
        free(mapping->function_name);
        free(mapping->filename);
        free(mapping->source_line);
        free(mapping->message_template);
        free(mapping->static_context);
        mapping->address_label = NULL;
        mapping->function_name = NULL;
        mapping->filename = NULL;
        mapping->source_line = NULL;
        mapping->message_template = NULL;
        mapping->static_context = NULL;
        return;
    }

    debug_info->runtime_trap_site_count++;
}


void debug_info_generate_dwarf(DebugInfo* debug_info, const char* output_filename) {
    if (!debug_info || !output_filename) return;
    
    FILE* file = fopen(output_filename, "w");
    if (!file) return;
    
    fprintf(file, "# DWARF Debug Information\n");
    fprintf(file, "# Generated for %s -> %s\n\n", 
            debug_info->source_filename ? debug_info->source_filename : "unknown",
            debug_info->assembly_filename ? debug_info->assembly_filename : "unknown");
    
    // Generate compilation unit header
    fprintf(file, ".section .debug_info\n");
    fprintf(file, ".long .Ldebug_info_end - .Ldebug_info_start\n");
    fprintf(file, ".Ldebug_info_start:\n");
    fprintf(file, ".short 4  # DWARF version\n");
    fprintf(file, ".long .debug_abbrev  # Abbreviation table offset\n");
    fprintf(file, ".byte 8  # Address size\n\n");
    
    for (size_t i = 0; i < debug_info->symbol_count; i++) {
        DebugSymbol* symbol = &debug_info->symbols[i];
        
        fprintf(file, "# Symbol: %s\n", symbol->name);
        fprintf(file, ".uleb128 1  # DW_TAG_variable\n");
        fprintf(file, ".string \"%s\"  # DW_AT_name\n", symbol->name);
        fprintf(file, ".long %zu  # DW_AT_decl_line\n", symbol->line);
        
        if (symbol->is_register) {
            fprintf(file, ".byte 0x50  # DW_OP_reg0 + register offset\n");
        } else {
            fprintf(file, ".byte 0x91  # DW_OP_fbreg\n");
            fprintf(file, ".sleb128 %d  # Stack offset\n", symbol->stack_offset);
        }
        fprintf(file, "\n");
    }
    
    fprintf(file, ".byte 0  # End of compilation unit\n");
    fprintf(file, ".Ldebug_info_end:\n\n");
    
    // Generate line number information
    fprintf(file, ".section .debug_line\n");
    fprintf(file, ".long .Ldebug_line_end - .Ldebug_line_start\n");
    fprintf(file, ".Ldebug_line_start:\n");
    fprintf(file, ".short 4  # DWARF version\n");
    fprintf(file, ".long .Ldebug_line_header_end - .Ldebug_line_header_start\n");
    fprintf(file, ".Ldebug_line_header_start:\n");
    fprintf(file, ".byte 1  # Minimum instruction length\n");
    fprintf(file, ".byte 1  # Default is_stmt\n");
    fprintf(file, ".byte -5  # Line base\n");
    fprintf(file, ".byte 14  # Line range\n");
    fprintf(file, ".byte 13  # Opcode base\n");
    
    // Standard opcode lengths
    for (int i = 1; i < 13; i++) {
        fprintf(file, ".byte %d\n", i == 1 ? 0 : 1);
    }
    
    fprintf(file, ".Ldebug_line_header_end:\n");
    
    // Generate line number program
    for (size_t i = 0; i < debug_info->mapping_count; i++) {
        SourceLineMapping* mapping = &debug_info->line_mappings[i];
        fprintf(file, ".byte 0  # Extended opcode\n");
        fprintf(file, ".uleb128 9  # Length\n");
        fprintf(file, ".byte 2  # DW_LNE_set_address\n");
        fprintf(file, ".quad .L%zu  # Assembly line %zu\n", mapping->assembly_line, mapping->assembly_line);
        fprintf(file, ".byte %zu  # Advance line to %zu\n", 
                mapping->source_line + 5, mapping->source_line);  // +5 for line base
    }
    
    fprintf(file, ".byte 0  # Extended opcode\n");
    fprintf(file, ".uleb128 1  # Length\n");
    fprintf(file, ".byte 1  # DW_LNE_end_sequence\n");
    fprintf(file, ".Ldebug_line_end:\n");
    
    fclose(file);
}

void debug_info_generate_stabs(DebugInfo* debug_info, const char* output_filename) {
    if (!debug_info || !output_filename) return;
    
    FILE* file = fopen(output_filename, "w");
    if (!file) return;
    
    fprintf(file, "# STABS Debug Information\n");
    fprintf(file, "# Generated for %s -> %s\n\n", 
            debug_info->source_filename ? debug_info->source_filename : "unknown",
            debug_info->assembly_filename ? debug_info->assembly_filename : "unknown");
    
    // Source file information
    if (debug_info->source_filename) {
        fprintf(file, ".stabs \"%s\",100,0,0,.Ltext0\n", debug_info->source_filename);
    }
    
    // Generate symbol information
    for (size_t i = 0; i < debug_info->symbol_count; i++) {
        DebugSymbol* symbol = &debug_info->symbols[i];
        
        const char* stab_type;
        switch (symbol->type) {
            case DEBUG_SYMBOL_VARIABLE:
                stab_type = "128";  // N_LSYM (local symbol)
                break;
            case DEBUG_SYMBOL_FUNCTION:
                stab_type = "36";   // N_FUN (function)
                break;
            case DEBUG_SYMBOL_PARAMETER:
                stab_type = "160";  // N_PSYM (parameter)
                break;
            default:
                stab_type = "128";
                break;
        }
        
        if (symbol->is_register) {
            fprintf(file, ".stabs \"%s:r%s\",%s,0,%zu,%s\n",
                    symbol->name,
                    symbol->type_name ? symbol->type_name : "int",
                    stab_type,
                    symbol->line,
                    symbol->register_name ? symbol->register_name : "eax");
        } else {
            fprintf(file, ".stabs \"%s:%s\",%s,0,%zu,%d\n",
                    symbol->name,
                    symbol->type_name ? symbol->type_name : "int",
                    stab_type,
                    symbol->line,
                    symbol->stack_offset);
        }
    }
    
    fclose(file);
}

void debug_info_generate_debug_map(DebugInfo* debug_info, const char* output_filename) {
    if (!debug_info || !output_filename) return;
    
    FILE* file = fopen(output_filename, "w");
    if (!file) return;
    
    fprintf(file, "# Debug Symbol Map\n");
    fprintf(file, "# Source: %s\n", 
            debug_info->source_filename ? debug_info->source_filename : "unknown");
    fprintf(file, "# Assembly: %s\n\n", 
            debug_info->assembly_filename ? debug_info->assembly_filename : "unknown");
    
    fprintf(file, "SYMBOLS:\n");
    for (size_t i = 0; i < debug_info->symbol_count; i++) {
        DebugSymbol* symbol = &debug_info->symbols[i];
        
        fprintf(file, "%s\t%s\t%zu:%zu\t", 
                symbol->name,
                symbol->type_name ? symbol->type_name : "unknown",
                symbol->line, symbol->column);
        
        if (symbol->is_register) {
            fprintf(file, "register:%s\n", 
                    symbol->register_name ? symbol->register_name : "unknown");
        } else {
            fprintf(file, "stack:%d\tsize:%zu\n", symbol->stack_offset, symbol->size);
        }
    }
    
    fprintf(file, "\nLINE_MAPPINGS:\n");
    for (size_t i = 0; i < debug_info->mapping_count; i++) {
        SourceLineMapping* mapping = &debug_info->line_mappings[i];
        fprintf(file, "%zu:%zu\t->\tasm:%zu\t%s\n",
                mapping->source_line, mapping->source_column,
                mapping->assembly_line,
                mapping->filename ? mapping->filename : "");
    }
    
    fclose(file);
}
