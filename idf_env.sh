# Source this file before building:  . ./idf_env.sh
# Uses $IDF_PATH if set, otherwise ~/esp/esp-idf (ESP-IDF v5.5.x).
if [ -d /opt/homebrew/opt/python@3.12/libexec/bin ]; then
  export PATH=/opt/homebrew/opt/python@3.12/libexec/bin:$PATH
fi
. "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null
