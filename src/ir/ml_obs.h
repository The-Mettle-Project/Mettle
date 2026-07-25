/* Observational-equivalence node features (OBS). See ml_obs.c for the design and
 * tools/mlopt/obs.py for the authoritative specification -- the two must agree
 * bit for bit or the model reads different inputs at compile time than it
 * trained on. ml_obs_selftest() checks that against tools/mlopt/obs_golden.txt.
 */
#ifndef ML_OBS_H
#define ML_OBS_H

#include <stdint.h>

#define ML_OBS_NPROBE 8                              /* probes per function */
#define ML_OBS_NBITS (ML_OBS_NPROBE * 64)            /* fingerprint bits */
#define ML_OBS_NPROJ 32                              /* SimHash dims */
#define ML_OBS_NSEM 4                                /* derived scalars */
#define ML_OBS_NOBS (ML_OBS_NPROJ + ML_OBS_NSEM)     /* features per node */

typedef struct {
  uint64_t v[ML_OBS_NPROBE];
  int valid;                 /* 0 when the node has no evaluable value */
} MlObsFp;

/* Per-instruction fingerprints. `fps` must hold n entries. When `leaves` and
 * `nleaves` are non-NULL they receive a malloc'd array of the fingerprints of
 * names whose values were invented rather than derived (params, globals, call
 * results, loads, mutable locals); the caller frees it. */
int ml_obs_fingerprints(char **texts, int n, MlObsFp *fps,
                        MlObsFp **leaves, int *nleaves);

/* Per-instruction features: ML_OBS_NPROJ projection dims then ML_OBS_NSEM
 * scalars (is_const, eq_leaf, eq_zero, dup_earlier). `out` holds n*ML_OBS_NOBS
 * floats and is fully written (zeros for nodes without a fingerprint). */
void ml_obs_features(char **texts, int n, float *out);

/* Value-equality edges: nearest earlier node with an identical fingerprint.
 * `src` and `dst` must each hold n entries; returns the edge count. */
int ml_obs_semantic_edges(char **texts, int n, int *src, int *dst);

/* Is this value worth linking as a reuse candidate? False for constants (which
 * would form cliques and are not a reuse opportunity) and for booleans (one bit
 * of identity, so unrelated comparisons collide). */
int ml_obs_edge_eligible(const MlObsFp *f);

/* Primitives, exposed for the golden-vector self-test. */
uint64_t ml_obs_splitmix64(uint64_t x);
uint64_t ml_obs_fnv1a64(const char *s);
void ml_obs_projection_row(int r, uint64_t out[ML_OBS_NPROBE]);

/* Replay tools/mlopt/obs_golden.txt. Returns 0 on full agreement, else the
 * number of mismatches, printing the first few to stderr. */
int ml_obs_selftest(const char *golden_path);

#endif /* ML_OBS_H */
