# hmac

HMAC-SHA-256 on top of the `sha256` package.

```mettle
import "hmac";

fn main() -> int32 {
  var tag: uint8[32];
  hmac_sha256_text("key", "message", &tag[0]);
  return 0;
}
```

`hmac_equal` compares two tags in time that does not depend on where they
differ. Use it to check a tag you received.

`mettle test hmac.mettle` runs RFC 4231 test cases 1, 2, 3 and 6. Case 6 uses a
key longer than one block.
