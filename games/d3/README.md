# Diablo III: Eternal Collection (macOS Native Port)

Native Apple Silicon macOS port of **Diablo III: Eternal Collection** (`01001B300B9BE000`, Update 2.7.6 / `v1441792` `01001B300B9BE800`).

## Inputs

- Base NSP: `01001B300B9BE000` (`[v0]`, ~13.3 GB)
- Update NSP: `01001B300B9BE800` (`[v1441792]`, ~3.59 GB)
- `prod.keys` and Switch firmware shared fonts (`0-standard.bfttf` .. `5-nintendo-extension.bfttf`)

## Build and Package

```sh
cd games/d3
make prepare runtime app WORK=D3_build \
  GAME="/path/to/update.nsp" KEYS="/path/to/prod.keys" \
  APP="Diablo III.app"
```

## Where things are

- Saves: `~/Library/Application Support/MacNativeGames/D3/profile`
- Controller type: `~/Library/Application Support/MacNativeGames/D3/controller-type.txt`
- Log: `~/Library/Logs/MacNativeGames/d3.log`
