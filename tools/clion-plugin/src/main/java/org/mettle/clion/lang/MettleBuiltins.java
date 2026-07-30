package org.mettle.clion.lang;

import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.Set;

/**
 * Names with built-in meaning that are not reserved words (docs/lexical-structure.md). They never
 * resolve to a declaration, so highlighting and completion have to know them by hand.
 */
public final class MettleBuiltins {

    /** Type names that are not keywords. */
    public static final Set<String> TYPES = set("bool", "cstring", "void", "Self", "Fn");

    /** Compile-time forms and constants. */
    public static final Set<String> VALUES = set("true", "false", "sizeof", "assert", "assert_eq");

    /** GPU built-ins available without declaration inside a {@code kernel}. */
    public static final Set<String> GPU = set(
            "thread", "block", "block_dim", "grid_dim",
            "subgroup_local_id", "subgroup_size", "subgroup_broadcast", "subgroup_shuffle",
            "subgroup_ballot", "subgroup_any", "subgroup_all",
            "subgroup_add", "subgroup_min", "subgroup_max",
            "subgroup_inclusive_add", "subgroup_exclusive_add",
            "atomic_load", "atomic_store", "atomic_exchange", "atomic_compare_exchange",
            "atomic_fetch_add", "atomic_fetch_sub", "atomic_fetch_min", "atomic_fetch_max",
            "atomic_fetch_and", "atomic_fetch_or", "atomic_fetch_xor",
            "tensor_mma", "tensor_matmul", "tensor_epilogue");

    /** Function decorators, without the {@code @}. */
    public static final Set<String> DECORATORS =
            set("inline", "noinline", "pure", "noalloc", "simd", "test");

    private MettleBuiltins() {
    }

    public static boolean isBuiltin(String name) {
        return TYPES.contains(name) || VALUES.contains(name) || isGpuBuiltin(name);
    }

    public static boolean isGpuBuiltin(String name) {
        if (name == null) return false;
        return GPU.contains(name)
                || name.startsWith("subgroup_") || name.startsWith("atomic_") || name.startsWith("tensor_");
    }

    private static Set<String> set(String... names) {
        return new LinkedHashSet<>(Arrays.asList(names));
    }
}
