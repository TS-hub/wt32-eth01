#!/usr/bin/env bash
IDF_PATH=/mnt/c/Users/Thomas/esp/v5.4/esp-idf
export IDF_PATH

# Strip CRLF and fix BASH_SOURCE detection (broken when sourced from /tmp)
sed -e 's/\r//g' \
    -e 's|idf_path=\$(dirname "\${BASH_SOURCE\[0\]}")|idf_path=/mnt/c/Users/Thomas/esp/v5.4/esp-idf|' \
    "$IDF_PATH/export.sh" > /tmp/idf_export_fixed.sh

. /tmp/idf_export_fixed.sh

cd /mnt/c/Users/Thomas/workspaces/wt32-eth01
# Call idf.py via python3 directly to bypass CRLF shebang issue
python3 "$IDF_PATH/tools/idf.py" build 2>&1 | tail -40
