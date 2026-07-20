#ifndef BASH_H_
#define BASH_H_

#include <string_view>

namespace ghatothkacha {

constexpr std::string_view kBashPreExec = R"bash(
  # start the daemon, if the daemon is already running this command will exit silently
  (command -v ghatothkacha >/dev/null && ghatothkacha --daemon &)

  _history_preexec() {
      local ret_code=$?

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

      # 1. Clean SQL: No more character replacements!
      export __GHAT_SQL_GLOBAL="SELECT cmd FROM (SELECT cmd, end_timestamp_ns FROM History ORDER BY end_timestamp_ns DESC LIMIT 10000) GROUP BY cmd ORDER BY MAX(end_timestamp_ns) DESC;"
      export __GHAT_SQL_DIR="SELECT cmd FROM (SELECT cmd, end_timestamp_ns FROM History WHERE dir = '${safe_pwd}' ORDER BY end_timestamp_ns DESC LIMIT 10000) GROUP BY cmd ORDER BY MAX(end_timestamp_ns) DESC;"

      # 2. SQLite ASCII Mode: Output rows separated by \x1E (Record Separator)
      # We pipe via printf so SQLite sees `.mode ascii` as a top-level command.
      # Then use tr to convert \x1E (\036 in octal) to NUL (\0) for fzf --read0
      export __GHAT_RELOAD_GLOBAL="printf '.mode ascii\n%s\n' \"\$__GHAT_SQL_GLOBAL\" | sqlite3 '${db_path}' | tr '\036' '\0'"
      export __GHAT_RELOAD_DIR="printf '.mode ascii\n%s\n' \"\$__GHAT_SQL_DIR\" | sqlite3 '${db_path}' | tr '\036' '\0'"

      local initial_cmd initial_label
      if [[ "$mode" == "global" ]]; then
          initial_cmd="$__GHAT_RELOAD_GLOBAL"
          initial_label="Global"
      else
          initial_cmd="$__GHAT_RELOAD_DIR"
          initial_label="Directory"
      fi

      # 3. fzf --read0 automatically supports multi-line items.
      # Added --highlight-line so the whole multi-line block is highlighted.
      local output
      output=$(eval "$initial_cmd" | \
        fzf --read0 --height 50% --reverse \
            --highlight-line \
            --border=rounded \
            --info=inline-right \
            --border-label=" $initial_label " \
            --expect=tab,enter \
            --bind="alt-d:reload(eval \"\$__GHAT_RELOAD_DIR\")+change-border-label( Directory )" \
            --bind="alt-g:reload(eval \"\$__GHAT_RELOAD_GLOBAL\")+change-border-label( Global )" \
            --footer=" Alt+g: Global • Alt+d: Dir • Tab: Edit • Enter: Run " \
            --footer-border=dashed \
            --scrollbar="│" \
            -q "$READLINE_LINE")

      local key="${output%%$'\n'*}"
      local command="${output#*$'\n'}"

      # 4. No cleanup needed! $command natively holds perfect multi-line strings
      if [[ -n "$command" && "$output" != "$command" ]]; then
          READLINE_LINE="$command"
          READLINE_POINT=${#READLINE_LINE}
          
          if [[ "$key" == "enter" ]]; then
              bind '"\e[0n": accept-line'
          else
              bind '"\e[0n": ""'
          fi
      else
          bind '"\e[0n": ""'
      fi
  }

  _fzf_global_history_search() { _ghatothkacha_fzf_core "global"; }
  _fzf_dir_history_search() { _ghatothkacha_fzf_core "dir"; }

  preexec_functions+=(_history_preexec)
  precmd_functions+=(_history_precmd)

  # --- THE MACRO TRICK ---
  bind -x '"\e[1n": _fzf_global_history_search'
  bind -x '"\e[2n": _fzf_dir_history_search'

  bind '"\C-r": "\e[1n\e[0n"'
  bind '"\C-h": "\e[1n\e[0n"'
  bind '"\e[A": "\e[2n\e[0n"'
  bind '"\eOA": "\e[2n\e[0n"'
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
