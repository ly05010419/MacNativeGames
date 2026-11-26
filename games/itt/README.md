# It Takes Two (macOS Native Port)

Native Apple Silicon macOS port of **It Takes Two** (`010092A0172E4000`, Update `v262144` / `v327680` `010092A0172E4800`).

## Inputs

- Base NSP: `010092A0172E4000` (`[v0]`)
- Update NSP: `010092A0172E4800`
- `prod.keys` and Switch firmware shared fonts (`0-standard.bfttf` .. `5-nintendo-extension.bfttf`)

## Build and Package

```sh
cd games/itt
make prepare runtime app WORK=ITT_build \
  GAME="/path/to/update.nsp" KEYS="/path/to/prod.keys" \
  APP="It Takes Two.app"
```

## Where things are

- Saves: `~/Library/Application Support/MacNativeGames/ITT/profile`
- Controller type: `~/Library/Application Support/MacNativeGames/ITT/controller-type.txt`
- Log: `~/Library/Logs/MacNativeGames/itt.log`
