# sha256

FIPS 180-4 SHA-256. No allocation and no imports.

```mettle
import "sha256";

fn main() -> int32 {
  var digest: uint8[32];
  sha256_text("abc", &digest[0]);

  var hex: uint8[65];
  sha256_hex(&digest[0], &hex[0]);
  return 0;
}
```

Hash a stream by calling `sha256_update` as often as the data arrives:

```mettle
var state: Sha256;
sha256_init(&state);
sha256_update(&state, first, first_length);
sha256_update(&state, second, second_length);
sha256_final(&state, &digest[0]);
```

`mettle test sha256.mettle` runs the NIST vectors and checks that a split
update gives the same digest as one call.
