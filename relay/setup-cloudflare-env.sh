#!/usr/bin/env bash
# Writes ~/.config/materializr-relay/env with your Cloudflare API token and
# account ID, readable only by you. Run it in a normal terminal (it prompts):
#
#     bash ~/Documents/Code/materializr/relay/setup-cloudflare-env.sh
#
# The token is typed hidden, so it never lands in your shell history.
set -euo pipefail

DIR="$HOME/.config/materializr-relay"
FILE="$DIR/env"

mkdir -p "$DIR"
chmod 700 "$DIR"

if [ -s "$FILE" ]; then
  read -rp "$FILE already has content. Overwrite it? [y/N] " yn
  case "$yn" in y|Y) ;; *) echo "Left it alone."; exit 0 ;; esac
fi

read -rsp "Cloudflare API token (hidden, then Enter): " CF_TOKEN
echo
read -rp  "Cloudflare Account ID: " CF_ACCT

if [ -z "$CF_TOKEN" ] || [ -z "$CF_ACCT" ]; then
  echo "Both values are required. Nothing written." >&2
  exit 1
fi

umask 077
printf 'CLOUDFLARE_API_TOKEN=%s\nCLOUDFLARE_ACCOUNT_ID=%s\n' "$CF_TOKEN" "$CF_ACCT" > "$FILE"
chmod 600 "$FILE"
unset CF_TOKEN CF_ACCT

echo
ls -l "$FILE"
echo "Written. Only you can read it."
