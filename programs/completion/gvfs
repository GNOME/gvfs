# Copyright (C) 2006-2007 Red Hat, Inc.
#
# This library is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as
# published by the Free Software Foundation; either version 2 of the
# License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307,
# USA.
#
# Author: David Zeuthen <davidz@redhat.com>

# Check for bash                                                                
[ -z "$BASH_VERSION" ] && return

####################################################################################################


__gvfs_multiple_uris() {
    local IFS=$'\n'
    local cur="${COMP_WORDS[COMP_CWORD]}"

    COMPREPLY=($(compgen -W '$(gvfs-ls --show-completions "$cur")' -- ""))

    # don't misbehave on colons; See item E13 at http://tiswww.case.edu/php/chet/bash/FAQ
    # We handle this locally be extracting any BLAH: prefix and removing it from the result.
    # Not great, but better than globally changing COMP_WORDBREAKS
    
    case "$cur" in
	*:*)
	    case "$COMP_WORDBREAKS" in
		*:*) colon_prefix=$(echo $cur | sed 's/:[^:]*$/:/' )
		    COMPREPLY=${COMPREPLY##${colon_prefix}}
		    ;;
	    esac
	    ;;
    esac
}

####################################################################################################

complete -o nospace -F __gvfs_multiple_uris gvfs-ls
complete -o nospace -F __gvfs_multiple_uris gvfs-info
complete -o nospace -F __gvfs_multiple_uris gvfs-cat
complete -o nospace -F __gvfs_multiple_uris gvfs-less
complete -o nospace -F __gvfs_multiple_uris gvfs-copy
complete -o nospace -F __gvfs_multiple_uris gvfs-mkdir
complete -o nospace -F __gvfs_multiple_uris gvfs-monitor-dir
complete -o nospace -F __gvfs_multiple_uris gvfs-monitor-file
complete -o nospace -F __gvfs_multiple_uris gvfs-move
complete -o nospace -F __gvfs_multiple_uris gvfs-open
complete -o nospace -F __gvfs_multiple_uris gvfs-rm
complete -o nospace -F __gvfs_multiple_uris gvfs-save
complete -o nospace -F __gvfs_multiple_uris gvfs-trash
complete -o nospace -F __gvfs_multiple_uris gvfs-tree
