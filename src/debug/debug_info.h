#ifndef DEBUG_INFO_H
#define DEBUG_INFO_H

#include <stddef.h>
#include <stdint.h>
#include "../parser/ast.h"

typedef enum {
    DEBUG_SYMBOL_VARIABLE,
    DEBUG_SYMBOL_FUNCTION,
    DEBUG_SYMBOL_PARAMETER,
    DEBUG_SYMBOL_STRUCT,
    DEBUG_SYMBOL_FIELD
} DebugSymbolType;

typedef struct {
    char* name;
    DebugSymbolType type;
    char* type_name;
    size_t line;
    size_t column;
    size_t address;  // Memory address or offset
    size_t size;     // Size in bytes
    int is_register; // 1 if stored in register, 0 if in memory
    char* register_name; // Register name if is_register is 1
    int stack_offset;    // Stack offset if stored on stack
} DebugSymbol;

typedef struct {
    size_t source_line;
    size_t source_column;
    size_t assembly_line;
    char* filename;
} SourceLineMapping;

typedef struct {
    char* function_name;
    char* start_label;
    char* end_label;
    char* filename;
    size_t line;
    size_t column;
} RuntimeFunctionMapping;

typedef struct {
    char* function_name;
    char* address_label;
    char* filename;
    size_t line;
    size_t column;
} RuntimeLocationMapping;

typedef struct {
    char* address_label;
    uint32_t kind;
    char* function_name;
    char* filename;
    size_t line;
    size_t column;
    char* source_line;
    char* message_template;
    char* static_context;
} RuntimeTrapSiteMapping;

typedef struct {
    DebugSymbol* symbols;
    size_t symbol_count;
    size_t symbol_capacity;
    
    SourceLineMapping* line_mappings;
    size_t mapping_count;
    size_t mapping_capacity;

    RuntimeFunctionMapping* runtime_functions;
    size_t runtime_function_count;
    size_t runtime_function_capacity;

    RuntimeLocationMapping* runtime_locations;
    size_t runtime_location_count;
    size_t runtime_location_capacity;

    RuntimeTrapSiteMapping* runtime_trap_sites;
    size_t runtime_trap_site_count;
    size_t runtime_trap_site_capacity;
    
    char* source_filename;
    char* assembly_filename;
} DebugInfo;

DebugInfo* debug_info_create(const char* source_filename, const char* assembly_filename);
void debug_info_destroy(DebugInfo* debug_info);

void debug_info_add_symbol(DebugInfo* debug_info, const char* name, DebugSymbolType type,
                          const char* type_name, size_t line, size_t column);
void debug_info_set_symbol_stack_offset(DebugInfo* debug_info, const char* name, int stack_offset);
DebugSymbol* debug_info_find_symbol(DebugInfo* debug_info, const char* name);

void debug_info_add_line_mapping(DebugInfo* debug_info, size_t source_line, size_t source_column,
                                size_t assembly_line, const char* filename);

// Runtime crash-trace metadata
void debug_info_add_runtime_function_mapping(DebugInfo* debug_info,
                                             const char* function_name,
                                             const char* start_label,
                                             const char* end_label,
                                             const char* filename,
                                             size_t line, size_t column);
void debug_info_add_runtime_location_mapping(DebugInfo* debug_info,
                                             const char* function_name,
                                             const char* address_label,
                                             const char* filename,
                                             size_t line, size_t column);

void debug_info_add_runtime_trap_site_mapping(DebugInfo* debug_info,
                                              const char* address_label,
                                              uint32_t kind,
                                              const char* function_name,
                                              const char* filename,
                                              size_t line, size_t column,
                                              const char* source_line,
                                              const char* message_template,
                                              const char* static_context);

char* debug_info_read_source_line(const char* filename, size_t line_number);

void debug_info_generate_dwarf(DebugInfo* debug_info, const char* output_filename);
void debug_info_generate_stabs(DebugInfo* debug_info, const char* output_filename);
void debug_info_generate_debug_map(DebugInfo* debug_info, const char* output_filename);

#endif // DEBUG_INFO_H
