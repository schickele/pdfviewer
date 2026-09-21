#!/usr/bin/env bash
# Download the latest PDF.js release into ./pdfjs
#   ./scripts/fetch-pdfjs.sh            # "legacy" build (default): includes polyfills,
#                                       # works with the Chromium inside Qt WebEngine
#   ./scripts/fetch-pdfjs.sh modern     # no polyfills; needs a bleeding-edge Chromium
#   PDFJS_VERSION=5.4.149 ./scripts/fetch-pdfjs.sh
set -euo pipefail
cd "$(dirname "$0")/.."

flavor="${1:-legacy}"
version="${PDFJS_VERSION:-}"

if [[ -z "$version" ]]; then
    # The /releases/latest redirect avoids GitHub API rate limits.
    tag=$(curl -fsSLI -o /dev/null -w '%{url_effective}' \
          https://github.com/mozilla/pdf.js/releases/latest)
    version="${tag##*/v}"
fi

suffix="dist"; [[ "$flavor" == "legacy" ]] && suffix="legacy-dist"
url="https://github.com/mozilla/pdf.js/releases/download/v${version}/pdfjs-${version}-${suffix}.zip"

echo "Fetching PDF.js ${version} (${flavor})..."
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/pdfjs.zip" "$url"
rm -rf pdfjs && mkdir pdfjs
unzip -q "$tmp/pdfjs.zip" -d pdfjs
echo "Done: $(pwd)/pdfjs"
