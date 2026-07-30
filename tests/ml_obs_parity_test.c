/* OBS Python/C parity gate.
 *
 * The `--ml-opt` model is trained on node features computed by
 * tools/mlopt/obs.py and runs on node features computed by src/ir/ml_obs.c. If
 * those two ever disagree, nothing crashes and nothing warns: the model simply
 * reads different inputs at compile time than it trained on, and the only
 * symptom is optimization quality quietly getting worse. tools/mlopt/README.md
 * already flags this hazard for the nine original scalar features; OBS widens
 * the surface to a 64-bit PRNG, a name hash, a 32x512 projection matrix, and
 * full uint64 expression evaluation, which is far too much to keep in sync by
 * reading both listings.
 *
 * So the Python side emits golden vectors (tools/mlopt/obs_golden.py) and this
 * replays them. A divergence is a build failure, not a mystery.
 *
 * Regenerate the vectors after any change to the featurizer:
 *     python tools/mlopt/obs_golden.py
 */
#include "ir/ml_obs.h"

#include <stdio.h>

int main(int argc, char **argv) {
  const char *golden = argc > 1 ? argv[1] : "tools/mlopt/obs_golden.txt";
  FILE *probe = fopen(golden, "r");
  if (!probe) {
    /* A source tree without the vectors (or a packaging build) should not fail
     * the suite; the parity claim is simply unverified here. */
    printf("RESULT: SKIP (no %s)\n", golden);
    return 0;
  }
  fclose(probe);

  int bad = ml_obs_selftest(golden);
  if (bad != 0) {
    printf("RESULT: FAIL (%d mismatches)\n", bad);
    return 1;
  }
  printf("RESULT: PASS\n");
  return 0;
}
