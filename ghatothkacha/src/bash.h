#ifndef BASH_H_
#define BASH_H_

#include <string_view>

namespace ghatothkacha {

constexpr std::string_view kBashPreExec = R"bash(
  export GHATOTHKACHA_BIN="$(command -v ghatothkacha)"
  export TOOEY_BIN="$(command -v tooey)"

  _history_preexec() {
      local ret_code=$?
      local cmd="$1"
  
      [[ -z "$GHATOTHKACHA_BIN" ]] && return

      if [[ -n "${__MY_HIST_ID:-}" ]]; then
          local end_time=$(date +%s%N)
          "$GHATOTHKACHA_BIN" --update -i "$__MY_HIST_ID" -e "$end_time" -r "$ret_code"
          unset __MY_HIST_ID
      fi

      case "$cmd" in
        "hx"|"hx ."|"vim"|"vi"|"clear"|"exit"|"ls"|"ls -al"|"ls -alh"| \
        "chezmoi apply"|"chezmoi diff"|"chezmoi cd"|"reboot"|"logout"|"nano"| \
        "cd"|"cd .."|"z"|"z .."|".."|"l"|"yy"|"yazi"|"htop"|"git status"| \
        "git push"|"pwd"|"ghostty")
            return 0
            ;;
      esac

      # additional ignore list
      if [[ -n "$GHATOTHKACHA_USER_IGNORE" ]] && [[ ":$GHATOTHKACHA_USER_IGNORE:" == *":$cmd:"* ]]; then
          return 0
      fi

      if [[ -f /proc/sys/kernel/random/uuid ]]; then
          export __MY_HIST_ID=$(< /proc/sys/kernel/random/uuid)
      else
          export __MY_HIST_ID=$(uuidgen)
      fi

      export __MY_HIST_START=$(date +%s%N)
      export __MY_HIST_CMD="$cmd"
      "$GHATOTHKACHA_BIN" --insert -i "$__MY_HIST_ID" -c "$__MY_HIST_CMD" -d "$PWD" -s "$__MY_HIST_START"
  }

  _history_precmd() {
      local ret_code=$?
      [[ -z "${__MY_HIST_ID:-}" ]] && return

      local end_time=$(date +%s%N)

      if [[ -n "$GHATOTHKACHA_BIN" ]]; then
          "$GHATOTHKACHA_BIN" --update -i "$__MY_HIST_ID" -e "$end_time" -r "$ret_code"
      fi
      unset __MY_HIST_ID
      unset __MY_HIST_CMD
      unset __MY_HIST_START
  }

  _ghatothkacha_ui() {
      local mode="$1"
      local current_query="$2"

      local db_path=$("$GHATOTHKACHA_BIN" --print-db-path)
      local safe_pwd="${PWD//\'/\'\'}"
      local history_limit="${GHATOTHKACHA_HISTORY_LIMIT:-10000}"

      local where_clause="" PROMPT NEXT_MODE
      if [[ "$mode" == "global" ]]; then
          PROMPT="[   Global  ] > "
          NEXT_MODE="dir"
      else
          local safe_pwd="${PWD//\'/\'\'}"
          where_clause=" WHERE dir = '${safe_pwd}'"
          PROMPT="[ Directory ] > "
          NEXT_MODE="global"
      fi

      local sql
      sql=$(cat <<SQL
        WITH CurrentTime AS (SELECT strftime('%s','now') AS now_sec)
        SELECT
          CASE WHEN retcode = 0 THEN char(27)||'[32m' ELSE char(27)||'[31m' END ||
          printf('%5s', CASE
            WHEN (MAX(end_timestamp_ns) - start_timestamp_ns) < 1000000000
              THEN ((MAX(end_timestamp_ns) - start_timestamp_ns)/1000000) || 'ms'
            WHEN (MAX(end_timestamp_ns) - start_timestamp_ns) < 60000000000
              THEN ((MAX(end_timestamp_ns) - start_timestamp_ns)/1000000000) || 's'
            ELSE ((MAX(end_timestamp_ns) - start_timestamp_ns)/60000000000) || 'm'
          END) || char(27)||'[0m ' ||
          char(27)||'[36m' ||
          printf('%10s', CASE
            WHEN now_sec - (MAX(end_timestamp_ns)/1000000000) < 60
              THEN (now_sec - (MAX(end_timestamp_ns)/1000000000)) || 's ago'
            WHEN now_sec - (MAX(end_timestamp_ns)/1000000000) < 3600
              THEN ((now_sec - (MAX(end_timestamp_ns)/1000000000))/60) || 'm ago'
            WHEN now_sec - (MAX(end_timestamp_ns)/1000000000) < 86400
              THEN ((now_sec - (MAX(end_timestamp_ns)/1000000000))/3600) || 'h ago'
            WHEN now_sec - (MAX(end_timestamp_ns)/1000000000) < 2592000
              THEN ((now_sec - (MAX(end_timestamp_ns)/1000000000))/86400) || 'd ago'
            WHEN now_sec - (MAX(end_timestamp_ns)/1000000000) < 31536000
              THEN round((now_sec - (MAX(end_timestamp_ns)/1000000000))/2592000.0, 1) || 'mo ago'
            ELSE round((now_sec - (MAX(end_timestamp_ns)/1000000000))/31536000.0, 1) || 'yr ago'
          END) || char(27)||'[0m ' || char(31) || cmd
        FROM (SELECT cmd, start_timestamp_ns, end_timestamp_ns, retcode
              FROM History${where_clause}
              ORDER BY end_timestamp_ns DESC LIMIT ${history_limit})
        CROSS JOIN CurrentTime
        GROUP BY cmd
        ORDER BY MAX(end_timestamp_ns) DESC;
SQL
      )

      # tooey inherently applies 'ShellEscapeInPlace' to {{@QUERY@}}, making it safe 
      # to inject as a direct argument into our toggle function without quotes.
      printf '.mode ascii\n%s\n' "$sql" | sqlite3 "$db_path" | tr '\036' '\0' | \
          "$TOOEY_BIN" --read0 --preview-size 40 \
          --prompt "$PROMPT" \
          --query-process-command "gai --no-color --read0 -f {{@QUERY@}}" \
          --ansi --tab-accept --height 50 \
          --preview-command "echo {{@SELECTION@}} | bat --wrap=auto --terminal-width=80" \
          --preview-dir bottom --print-key \
          --action "alt-t:Toggle==_ghatothkacha_ui $NEXT_MODE {{@QUERY@}}" \
          --footer "Alt+t: Toggle Global/Dir • Tab: Edit • Enter: Run" \
          --query "$current_query"
  }
  export -f _ghatothkacha_ui

  _ghatothkacha_history_core() {
      local mode="$1"
      local output
      
      output=$(_ghatothkacha_ui "$mode" "$READLINE_LINE")

      local key="${output%%$'\n'*}"
      local raw_line="${output#*$'\n'}"
      local command="${raw_line#*$'\x1f'}"

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

  _ghatothchaka_global_history_search() { _ghatothkacha_history_core "global"; }
  _ghatothchaka_dir_history_search() { _ghatothkacha_history_core "dir"; }

  preexec_functions+=(_history_preexec)
  precmd_functions+=(_history_precmd)

  # --- THE MACRO TRICK ---
  bind -x '"\e[1n": _ghatothchaka_global_history_search'
  bind -x '"\e[2n": _ghatothchaka_dir_history_search'

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

    local original_histtimeformat="${HISTTIMEFORMAT:-}"
    unset HISTTIMEFORMAT

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
                    id=$(< /proc/sys/kernel/random/uuid)
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
            id=$(< /proc/sys/kernel/random/uuid)
        else
            id=$(uuidgen)
        fi

        local ts=$((base_time + counter * 1000000))

        "$bin_path" --insert -i "$id" -c "$cmd_stripped" -d "$dir" -s "$ts"
        "$bin_path" --update -i "$id" -e "$ts" -r "0"

        counter=$((counter + 1))
    fi

    if [[ -n "${original_histtimeformat:-}" ]]; then
        export HISTTIMEFORMAT="$original_histtimeformat"
    fi

    echo "Successfully dispatched $counter history items."
}

_ghatothkacha_import_history
unset -f _ghatothkacha_import_history
)bash";

}  // namespace ghatothkacha

#endif  // BASH_H_
