# SuperTerminal zsh integration.
#
# st starts zsh with ZDOTDIR pointing at wrapper startup files that load the
# user's own, then this. It replaces the accept-line widget (Enter): each line
# is checked with zsh itself (zsh -n for syntax, the command tables for every
# word; nothing runs), and lines that aren't runnable commands, or read like
# English, are sent to st to be translated. The result comes back as a real
# command on the prompt and runs in this same shell.
#
# Talks to st through `$SUPERTERMINAL_EXE --client $SUPERTERMINAL_PIPE <type>`.

[[ -o interactive && -n $SUPERTERMINAL_PIPE && -n $SUPERTERMINAL_EXE ]] || return 0
zmodload zsh/parameter 2>/dev/null
autoload -Uz add-zsh-hook

typeset -g __st_exe=$SUPERTERMINAL_EXE __st_pipe=$SUPERTERMINAL_PIPE
typeset -g __st_suggestion= __st_lastline=
typeset -ga __st_hist

typeset -g __st_os
if [[ $OSTYPE == darwin* ]]; then
    __st_os="macOS $(sw_vers -productVersion 2>/dev/null)"
else
    __st_os=$( . /etc/os-release 2>/dev/null; print -rn -- "${PRETTY_NAME:-$(uname -s)}" )
    grep -qi microsoft /proc/version 2>/dev/null && __st_os+=" (WSL on Windows)"
fi

# __st_call <type> [key value]... : one request to st; prints its reply.
__st_call() {
    local type=$1
    shift
    { while (( $# >= 2 )); do printf '%s\x1f%s\x1e' "$1" "$2"; shift 2; done } |
        "$__st_exe" --client "$__st_pipe" "$type" 2>/dev/null
}

# "word<TAB>kind" for each word: alias, function, builtin, reserved, command, or none.
__st_types() {
    local s=${1//[;|&()<>]/ } w t
    local -A seen
    for w in ${=s}; do
        [[ -n ${seen[$w]} ]] && continue
        seen[$w]=1
        if (( ${+aliases[$w]} )); then t=alias
        elif (( ${+functions[$w]} )); then t=function
        elif (( ${+builtins[$w]} )); then t=builtin
        elif (( ${reswords[(Ie)$w]} )); then t=reserved
        elif (( ${+commands[$w]} )) || [[ $w == */* && -x $w ]]; then t=command
        else t=none
        fi
        print -r -- "$w"$'\t'"$t"
    done
}

# Fast path: the first word is a command and nothing reads like English.
__st_plain() {
    local line=$1 types=$2 w first
    [[ $line == '!!'* || $line == '??'* || $line == *'?' ]] && return 1
    local -a ws
    ws=(${=line})
    first=${ws[1]}
    [[ $'\n'$types == *$'\n'"$first"$'\t'none* ]] && return 1
    for w in ${ws[2,-1]}; do
        case ${(L)w} in
            the|a|an|all|my|me|these|those|this|that|them|it|its|please|every|some|any|what|which|how|why|who|is|are|can|could|would|should|than|there|here) return 1 ;;
        esac
        [[ $first == dir && $w == /[a-zA-Z]* && ${#w} -le 4 ]] && return 1
    done
    return 0
}

__st_listing() { CLICOLOR= LC_ALL=C command ls -lAh 2>/dev/null | head -n 81 }

__st_note() {  # colour text
    local line
    for line in "${(@f)2}"; do print -r -- $'\e['"$1"'m  '"$line"$'\e[0m'; done
}

__st_exit_widget() { print; exit 0 }
zle -N __st_exit_widget

__st_accept_line() {
    # Continuation lines, empty lines, and Enter on a suggested command: just run.
    if [[ -n $PREBUFFER || -z ${BUFFER//[[:space:]]/} ]]; then zle .accept-line; return; fi
    if [[ -n $__st_suggestion && $BUFFER == $__st_suggestion ]]; then
        __st_suggestion=
        zle .accept-line
        return
    fi
    __st_suggestion=

    local line=$BUFFER syntax=ok err
    if ! err=$(zsh -n -c "$line" 2>&1); then
        # zsh says "parse error near `then'" for unfinished input too; the token tells them apart.
        local tok=${err##*near \`}
        tok=${tok%\'*}
        if [[ $err == *unmatched* || $err == *"end of file"* ]]; then syntax=incomplete
        else
            case $tok in
                then|do|else|elif|in|'{'|'('|'|'|'||'|'&&') syntax=incomplete ;;
                *) syntax=error ;;
            esac
        fi
    fi
    local types=$(__st_types "$line")
    if [[ $syntax != error ]] && __st_plain "$line" "$types"; then zle .accept-line; return; fi

    local reply=$(__st_call route shell zsh line "$line" syntax "$syntax" types "$types")
    if [[ -z $reply ]]; then zle .accept-line; return; fi
    local action=${reply%%$'\n'*} rest=${reply#*$'\n'}
    local reason=${rest%%$'\n'*}
    rest=${rest#*$'\n'}
    if [[ $action != ai ]]; then
        [[ -n $rest && $rest != $line ]] && BUFFER=$rest
        zle .accept-line
        return
    fi

    # Work below the typed line.
    zle -I
    print
    local out=$(mktemp "${TMPDIR:-/tmp}/st.XXXXXX")
    ( __st_call ask shell zsh shellVersion "$ZSH_VERSION" request "$rest" reason "$reason" cwd "$PWD" \
        home "$HOME" os "$__st_os" listing "$(__st_listing)" history "${(F)__st_hist}" >"$out" ) &!
    local pid=$! key dots=1
    while kill -0 $pid 2>/dev/null; do
        printf '\r\e[2K\e[90m  %s\e[0m' "${(l:$dots::.:)}"
        dots=$(( dots % 3 + 1 ))
        if read -s -t 0.15 -k 1 key; then
            case $key in
                $'\e')
                    kill $pid 2>/dev/null
                    printf '\r\e[2K'
                    rm -f "$out"
                    zle reset-prompt
                    return ;;
                $'\x03'|$'\x18')
                    kill $pid 2>/dev/null
                    rm -f "$out"
                    print
                    exit 0 ;;
            esac
        fi
    done
    printf '\r\e[2K'

    local reply2=$(<"$out")
    rm -f "$out"
    local kind=${reply2%%$'\n'*}
    reply2=${reply2#*$'\n'}
    local autorun=${reply2%%$'\n'*}
    reply2=${reply2#*$'\n'}
    local note=${reply2%%$'\n'*}
    local payload=${reply2#*$'\n'}

    case $kind in
        command)
            if [[ -n $note ]]; then
                case $note in
                    *deletes*) __st_note 91 "$note" ;;
                    *changes*) __st_note 93 "$note" ;;
                    *) __st_note 90 "$note" ;;
                esac
            fi
            print -s -- "$line"
            BUFFER=$payload
            CURSOR=$#BUFFER
            if [[ $autorun == 1 ]]; then
                zle .accept-line
            else
                __st_suggestion=$payload
                zle reset-prompt
            fi ;;
        answer)
            __st_note 0 "$payload"
            print -s -- "$line"
            BUFFER=
            zle reset-prompt ;;
        passthrough)
            zle .accept-line ;;
        *)
            __st_note 91 "${payload:-No reply.}"
            zle reset-prompt ;;
    esac
}
zle -N accept-line __st_accept_line

# Ctrl+X at the prompt exits; Ctrl+C at the prompt exits too (ZLE is active),
# while Ctrl+C during a running command keeps interrupting it.
bindkey '^X' __st_exit_widget
# Esc on its own clears the line (cancels a suggested command), as in PowerShell.
# Emacs keymap only: in vi mode Esc keeps its meaning. Alt+key still works (KEYTIMEOUT).
bindkey -M emacs '^[' kill-buffer
TRAPINT() {
    if zle 2>/dev/null; then print; exit 0; fi
    return $(( 128 + $1 ))
}

__st_precmd() {
    local st_status=$? last=$(fc -ln -1 2>/dev/null)
    if [[ -n $last && $last != $__st_lastline ]]; then
        __st_hist+=("$st_status"$'\t'"$last")
        (( ${#__st_hist} > 10 )) && shift __st_hist
        __st_lastline=$last
    fi
    return $st_status
}
add-zsh-hook precmd __st_precmd

st() {
    case ${1:-help} in
        model|effort) __st_call config key "$1" value "$2" ;;
        auto|autorun) __st_call config key autorun value "$2" ;;
        status|settings|config) __st_call config ;;
        *)
            print -P '%F{cyan}SuperTerminal%f: type commands as usual, or say what you want in plain English.'
            print
            print '  show me the biggest files here'
            print '  which process is using port 3000?'
            print -P '  dir /w                         %F{8}(PowerShell and cmd habits work too)%f'
            print -P '  fix it                         %F{8}(after a command fails)%f'
            print
            print '  ?? <text>          treat the line as plain English, even if it looks like a command'
            print '  !! <command>       run exactly as typed'
            print "  Esc                cancel a suggested command, or a request in progress"
            print '  Ctrl+C / Ctrl+X    exit SuperTerminal (Ctrl+C during a running command just stops it)'
            print
            print '  st status          settings'
            print '  st auto on|off     run read-only answers straight away, or always wait for Enter'
            print ;;
    esac
}

() {
    local hello=$(__st_call hello)
    [[ -z $hello ]] && return
    print -r -- $'\e[96mSuperTerminal\e[0m '"${hello%%$'\n'*}"$'  \e[90m·  zsh '"$ZSH_VERSION"$'\e[0m'
    if [[ ${hello#*$'\n'} == 1* ]]; then
        print -r -- $'\e[90mType commands as usual, or just say what you want. `st help` for more.\e[0m'
    else
        print -r -- $'\e[93mANTHROPIC_API_KEY isn\'t set, so plain-English requests are off.\e[0m'
    fi
}
