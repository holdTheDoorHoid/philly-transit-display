# SEPTA CA bundle - provenance and regeneration

`../ca_bundle.cpp` is a generated file (a C byte array) built from the five PEM
files in this directory. This document is what makes that array reproducible
and auditable instead of an opaque blob.

## Why a bundle at all, and why not the full Mozilla list

Arduino-ESP32 core 3.x's `NetworkClientSecure` (the class `WiFiClientSecure` is
now a compatibility alias for) exposes exactly one certificate-authority-bundle
API:

```cpp
void setCACertBundle(const uint8_t *bundle, size_t size);
```

Checked directly against `libraries/NetworkClientSecure/src/NetworkClientSecure.cpp`
at arduino-esp32 tag `3.3.11` (the core version pioarduino's `stable` platform
release currently ships, see `firmware/platformio.ini`): this method calls
`esp_crt_bundle_set(bundle, size)` followed by attaching ESP-IDF's
`esp_crt_bundle_attach` callback. There is **no** public method to say "use
whatever default bundle the framework happens to have baked in" - passing
`nullptr`/`0` explicitly *detaches* the bundle rather than falling back to a
default. In other words: if you want bundle-based verification from Arduino
code, you supply the bundle. The library's own `NetworkClientSecure/README.md`
confirms this ("if the Arduino IDE added support for embedding files... if
not, you have two choices: 1. create a makefile... 2. store the bundle as a
SPIFFS file"); we take a third option below (commit the generated array as
ordinary vendored source, same treatment as `firmware/boards/*.json`).

Rather than embedding the full ~130-cert Mozilla root list ESP-IDF's own
`gen_crt_bundle.py` normally builds (tens of KB, most of it irrelevant), this
project's threat model is narrow: DESIGN.md SS12 says "Firmware never contacts
anything except SEPTA and NTP" (NTP is plaintext, no TLS). So the bundle here
holds only the roots that can plausibly appear in SEPTA's chain, keeping it
small on a board with no PSRAM.

## What's in the bundle

Fetched 2026-09-13 from Amazon's own certificate repository
(`https://www.amazontrust.com/repository/<name>.pem`), the standard "Amazon
Trust Services" root set:

| File | Subject | SHA-256 fingerprint |
|---|---|---|
| `AmazonRootCA1.pem` | Amazon Root CA 1 | `8E:CD:E6:88:4F:3D:87:B1:12:5B:A3:1A:C3:FC:B1:3D:70:16:DE:7F:57:CC:90:4F:E1:CB:97:C6:AE:98:19:6E` |
| `AmazonRootCA2.pem` | Amazon Root CA 2 | `1B:A5:B2:AA:8C:65:40:1A:82:96:01:18:F8:0B:EC:4F:62:30:4D:83:CE:C4:71:3A:19:C3:9C:01:1E:A4:6D:B4` |
| `AmazonRootCA3.pem` | Amazon Root CA 3 | `18:CE:6C:FE:7B:F1:4E:60:B2:E3:47:B8:DF:E8:68:CB:31:D0:2E:BB:3A:DA:27:15:69:F5:03:43:B4:6D:B3:A4` |
| `AmazonRootCA4.pem` | Amazon Root CA 4 | `E3:5D:28:41:9E:D0:20:25:CF:A6:90:38:CD:62:39:62:45:8D:A5:C6:95:FB:DE:A3:C2:2B:0B:FB:25:89:70:92` |
| `SFSRootCAG2.pem` | Starfield Services Root Certificate Authority - G2 | `56:8D:69:05:A2:C8:87:08:A4:B3:02:51:90:ED:CF:ED:B1:97:4A:60:6A:13:C6:E5:29:0F:CB:2A:E6:3E:DA:B5` |

Verified live 2026-09-13 with:

```sh
echo | openssl s_client -connect www3.septa.org:443 -servername www3.septa.org -showcerts
```

`www3.septa.org`'s actual chain is `*.septa.org` -> `Amazon RSA 2048 M01` ->
`Amazon Root CA 1`, with `Amazon Root CA 1` cross-signed by the Starfield root
(older clients that don't trust the Amazon roots directly fall back to the
Starfield cross-sign). Trusting `Amazon Root CA 1` alone is sufficient for
today's chain; all five are included anyway so the bundle survives SEPTA/AWS
rotating to a different Amazon Trust root (2/3/4 are the EC alternates) without
a firmware update, matching DESIGN.md SS2's literal phrase "Amazon Trust
Services roots" rather than one specific cert.

`gen_crt_bundle.py` is vendored here as a straight copy from
`espressif/arduino-esp32` at tag `3.3.11`
(`tools/gen_crt_bundle.py`, Apache-2.0) so the bundle can be regenerated
without re-fetching it, and so the exact tool version used is pinned
alongside its output.

## Regenerating `ca_bundle.cpp`

Only needed if a root above is revoked/rotated, or another agency source is
added later (DESIGN.md SS11) with a different CA.

```sh
cd firmware/src/app/ca_bundle_src
python3 -m pip install --user cryptography   # gen_crt_bundle.py's only dependency
python3 gen_crt_bundle.py --input AmazonRootCA1.pem AmazonRootCA2.pem \
    AmazonRootCA3.pem AmazonRootCA4.pem SFSRootCAG2.pem
# writes ./x509_crt_bundle (Espressif's compact bundle format, not raw PEM)
python3 - <<'EOF'
data = open("x509_crt_bundle", "rb").read()
lines = ",\n".join(
    "  " + ", ".join(f"0x{b:02x}" for b in data[i:i+16])
    for i in range(0, len(data), 16)
)
open("../ca_bundle.cpp", "w").write(f'''// Generated file. See firmware/src/app/ca_bundle_src/README.md for provenance and how to regenerate.
// DO NOT hand-edit; regenerate with gen_crt_bundle.py per that README.
#include "ca_bundle.h"

namespace transit_app {{

const uint8_t kSeptaCaBundle[{len(data)}] = {{
{lines},
}};

const size_t kSeptaCaBundleLen = {len(data)};

}}  // namespace transit_app
''')
EOF
rm x509_crt_bundle
```

Then update the fingerprint table above if the cert set changed, and note the
new fetch date.
