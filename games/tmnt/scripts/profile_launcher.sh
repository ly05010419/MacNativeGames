# Persistent game data must never depend on a runtime binary's hash.
BASE="$HOME/Library/Application Support/MacNativeGames"
PROFILE="$BASE/TMNT"
mkdir -p "$BASE"
if [ ! -d "$PROFILE" ]; then
    LOCK="$BASE/.tmnt-profile-migration-lock"
    ATTEMPTS=0
    OWN_LOCK=0
    until [ "$OWN_LOCK" = 1 ] || [ -d "$PROFILE" ]; do
        if mkdir "$LOCK" 2>/dev/null; then
            OWN_LOCK=1
            trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT
            trap 'exit 1' HUP INT TERM
        else
            ATTEMPTS=$((ATTEMPTS + 1))
            if [ "$ATTEMPTS" -ge 30 ]; then
                printf '[tmnt] save migration is locked: %s. Close other TMNT launches; remove this empty lock if no migration is running.\n' "$LOCK" >&2
                exit 1
            fi
            sleep 1
        fi
    done
    if [ ! -d "$PROFILE" ]; then
        STAGING=$(mktemp -d "$BASE/.tmnt-profile.XXXXXX")
        NEWEST=''
        NEWEST_TIME=0
        for OLD in "$BASE"/TMNT-*; do
            [ -f "$OLD/account.uid" ] && [ -d "$OLD/save" ] || continue
            TIME=$(find "$OLD/save" -type f -exec stat -f '%m' {} + | sort -nr | head -1)
            [ -n "$TIME" ] || continue
            if [ -z "$NEWEST" ] || [ "$TIME" -gt "$NEWEST_TIME" ] || { [ "$TIME" -eq "$NEWEST_TIME" ] && [ "$OLD" -nt "$NEWEST" ]; }; then
                NEWEST="$OLD"
                NEWEST_TIME="$TIME"
            fi
        done
        if [ -n "$NEWEST" ]; then
            for ITEM in account.uid save host-settings.plist controller-type.txt controls.txt; do
                [ ! -e "$NEWEST/$ITEM" ] || cp -pR "$NEWEST/$ITEM" "$STAGING/$ITEM"
            done
            printf '%s\n' "$NEWEST" > "$STAGING/migrated-from.txt"
            printf '[tmnt] migrated saves from %s; original retained\n' "$NEWEST" >&2
        fi
        mv "$STAGING" "$PROFILE"
    fi
    if [ "$OWN_LOCK" = 1 ]; then
        rmdir "$LOCK"
        trap - EXIT HUP INT TERM
    fi
fi
