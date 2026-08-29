# Application gamepad routing

## Objective

Route Moonlight controller input according to the active Sunshine application:

| Application | Output |
| --- | --- |
| `Nintendo Switch` | Configured controller output, currently NXBT. |
| `Xbox` | Disabled until the Xbox Remote Play input sink exists. |
| `HDMI Input`, `Desktop`, and other applications | Disabled. |

## Implementation

- Add an explicit disabled gamepad-router mode which accepts controller lifecycle events without sending them to an output backend.
- Bind the selected router to each stream input when it is created. This preserves the original backend for controller neutralization and release when applications change.
- Select the router when an application launches and select the disabled router when it stops.
- Route to the configured output only when the application name is exactly `Nintendo Switch`; all other application names use the disabled router.
- Keep Xbox disabled until an Xbox Remote Play input sink is implemented.

## Validation

- Built successfully with `./scripts/build-rkmpp.sh`.
- Restarted Sunshine and verified that it initialized the RKMPP encoders and configuration UI with `controller_output = nxbt`.
- Added focused gamepad-router and input-session tests. The prescribed RKMPP build disables test targets, so its `test_sunshine` executable was not rebuilt or run as part of this validation.

## Runtime contract

The application name is read from `runtime-home/config/sunshine/apps.json`. If the Nintendo Switch entry is renamed, update the exact-name routing rule at the same time.
