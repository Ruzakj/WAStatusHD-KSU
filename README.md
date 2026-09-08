# WAStatusHD-KSU

Experimental KernelSU Next / Zygisk Next module targeting WhatsApp 2.26.34 (`com.whatsapp`).

Goal: isolate the WaEnhancer Media Quality behavior for WhatsApp Status/Story without LSPosed.

## Current build

- Target ABI: arm64-v8a
- Target package: com.whatsapp
- Target WhatsApp: 2.26.34
- Root/module manager: KernelSU Next
- Injection: Zygisk Next compatible Zygisk API v5
- Fail-safe: non-target processes unload immediately

## Status

`v0.1-alpha` is the standalone injector baseline. It verifies that the module loads only inside WhatsApp and provides the native hook bootstrap used for the Media Quality port. The Media Quality resolver is intentionally fail-open: unsupported WhatsApp internals must not crash WhatsApp.

The quality logic being ported from WaEnhancer includes Story HD availability, image quality 100, larger image edge/size limits, video max-edge/bitrate limits and transcoder flags.

## Build

GitHub Actions builds a KernelSU/Magisk-style module ZIP. Install Zygisk Next first, then install the generated ZIP from KernelSU Next and reboot.

## Upstream / license

Media Quality behavior is based on Dev4Mod/WaEnhancer (GPL-3.0). This derivative project is GPL-3.0.
