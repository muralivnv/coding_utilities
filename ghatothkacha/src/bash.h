#ifndef BASH_H_
#define BASH_H_

#include <string_view>

namespace ghatothkacha {

constexpr std::string_view kBashPreExec = R"bash(
  # start the daemon, if the daemon is already running this command will exit silently
  (command -v ghatothkacha >/dev/null && ghatothkacha --daemon &)

  _history_preexec() {
      local ret_code=$?

      # Send an --update for the PREVIOUS command before we generate a new UUID.
      if [[ -n "${__MY_HIST_ID:-}" ]]; then
          local end_time=$(date +%s%N)
          local bin_path=$(command -v ghatothkacha)
          if [[ -n "$bin_path" ]]; then
              "$bin_path" --update -i "$__MY_HIST_ID" -e "$end_time" -r "$ret_code"
          fi
      fi

      if [[ -f /proc/sys/kernel/random/uuid ]]; then
          export __MY_HIST_ID=$(cat /proc/sys/kernel/random/uuid)
      else
          export __MY_HIST_ID=$(uuidgen)
      fi
      
      export __MY_HIST_START=$(date +%s%N)
      export __MY_HIST_CMD="$1"

      local bin_path=$(command -v ghatothkacha)
      if [[ -n "$bin_path" ]]; then
          "$bin_path" --insert -i "$__MY_HIST_ID" -c "$__MY_HIST_CMD" -d "$PWD" -s "$__MY_HIST_START"
      fi
  }

  _history_precmd() {
      local ret_code=$? 
      if [[ -z "${__MY_HIST_ID:-}" ]]; then 
          return
      fi

      local end_time=$(date +%s%N)
      local bin_path=$(command -v ghatothkacha)

      if [[ -n "$bin_path" ]]; then
          "$bin_path" --update -i "$__MY_HIST_ID" -e "$end_time" -r "$ret_code"
      fi
      unset __MY_HIST_ID
      unset __MY_HIST_CMD
      unset __MY_HIST_START
  }

  _ghatothkacha_fzf_core() {
      local mode="$1"
      local db_path=$(ghatothkacha --print-db-path)
      local safe_pwd="${PWD//\'/\'\'}"

      # 1. Define the raw SQL queries
      local global_sql="SELECT cmd FROM (SELECT cmd, end_timestamp_ns FROM History ORDER BY end_timestamp_ns DESC LIMIT 10000) GROUP BY cmd ORDER BY MAX(end_timestamp_ns) DESC;"
      local dir_sql="SELECT cmd FROM (SELECT cmd, end_timestamp_ns FROM History WHERE dir = '${safe_pwd}' ORDER BY end_timestamp_ns DESC LIMIT 10000) GROUP BY cmd ORDER BY MAX(end_timestamp_ns) DESC;"

      # 2. Package them into executable strings for fzf's reload command
      # We wrap the db_path in single quotes, and the SQL in double quotes for sh -c
      local reload_global="sqlite3 '${db_path}' \"${global_sql}\""
      local reload_dir="sqlite3 '${db_path}' \"${dir_sql}\""

      local initial_cmd initial_label
      if [[ "$mode" == "global" ]]; then
          initial_cmd="$reload_global"
          initial_label="Global"
      else
          initial_cmd="$reload_dir"
          initial_label="Directory"
      fi

      # 3. Run fzf with dynamic bindings and key expectations
      local output
      b=$'\033[1m'
      n=$'\033[0m'
      output=$(eval "$initial_cmd" | \
        fzf --height 40% --reverse \
            --border=rounded \
            --info=inline-right \
            --prompt="$initial_prompt" \
            --border-label=" $initial_label " \
            --bind="alt-d:reload($reload_dir)+change-border-label( Directory )" \
            --bind="alt-g:reload($reload_global)+change-border-label( Global )" \
            --bind="tab:down,shift-tab:up" \
            --footer=" Alt + g: GlobalHistorySearch • d: DirHistorySearch " \
            --footer-border=dashed \
            --scrollbar="│" \
            -q "$READLINE_LINE")

      if [[ -n "$output" ]]; then
          READLINE_LINE="$output"
          READLINE_POINT=${#READLINE_LINE}
      fi
  }

  _fzf_global_history_search() { _ghatothkacha_fzf_core "global"; }
  _fzf_dir_history_search() { _ghatothkacha_fzf_core "dir"; }

  preexec_functions+=(_history_preexec)
  precmd_functions+=(_history_precmd)

  bind -x '"\C-r": _fzf_global_history_search'
  bind -x '"\C-h": _fzf_global_history_search'

  # Bind to the Up Arrow.
  # the Up arrow sends one of these two escape sequences. Bind both to be safe.
  bind -x '"\e[A": _fzf_dir_history_search'
  bind -x '"\eOA": _fzf_dir_history_search'
)bash";

constexpr std::string_view kHistoryImport = R"bash(
_ghatothkacha_import_history() {
    echo "Importing bash history into ghatothkacha..."

    local bin_path=$(command -v ghatothkacha)
    if [[ -z "$bin_path" ]]; then
        echo "Error: ghatothkacha binary not found in PATH."
        return 1
    fi

    # Ensure the daemon is running
    ("$bin_path" --daemon &)

    local current_cmd=""
    local counter=0
    # Use current time in nanoseconds as a base to ensure chronological sorting
    local base_time=$(date +%s%N)
    local dir="unknown"

    while IFS= read -r line; do
        # Regex checks if the line starts with spaces, a number, an optional '*' (if modified), and spaces
        if [[ "$line" =~ ^[[:space:]]*[0-9]+\*?[[:space:]]+(.*)$ ]]; then

            if [[ -n "$current_cmd" ]]; then
                # Strip the trailing newline bash adds to our buffer
                local cmd_stripped="${current_cmd%$'\n'}"

                local id
                if [[ -f /proc/sys/kernel/random/uuid ]]; then
                    id=$(cat /proc/sys/kernel/random/uuid)
                else
                    id=$(uuidgen)
                fi

                local ts=$((base_time + counter * 1000000)) # Increment by 1ms to maintain order

                "$bin_path" --insert -i "$id" -c "$cmd_stripped" -d "$dir" -s "$ts"
                "$bin_path" --update -i "$id" -e "$ts" -r "0"

                counter=$((counter + 1))
            fi

            # Start new buffer using the captured command
            current_cmd="${BASH_REMATCH[1]}"$'\n'
        else
            # Append multi-line command parts
            current_cmd+="$line"$'\n'
        fi
    done < <(history)

    # Insert the final command left in the buffer
    if [[ -n "$current_cmd" ]]; then
        local cmd_stripped="${current_cmd%$'\n'}"
        
        local id
        if [[ -f /proc/sys/kernel/random/uuid ]]; then
            id=$(cat /proc/sys/kernel/random/uuid)
        else
            id=$(uuidgen)
        fi

        local ts=$((base_time + counter * 1000000))

        "$bin_path" --insert -i "$id" -c "$cmd_stripped" -d "$dir" -s "$ts"
        "$bin_path" --update -i "$id" -e "$ts" -r "0"

        counter=$((counter + 1))
    fi

    echo "Successfully dispatched $counter history items."
}

_ghatothkacha_import_history
unset -f _ghatothkacha_import_history
)bash";

}  // namespace ghatothkacha

#endif  // BASH_H_
