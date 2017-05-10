#!/bin/bash

cd "$(dirname "$0")"/..

version_file=src/rvt_lib/version.hpp
current_version=$(sed -E '/#d/!d;s/.*"([^"]+)".*/\1/' "$version_file")
progname="$0"

usage ()
{
    echo 'usage:' >&2
    echo "  $progname -u,--update-version new-version [-f,--force-version] [-p,--push] [-P,--packager path] [-- packager_args...]" >&2
    echo "  $progname -g,--get-version" >&2
    echo "current version: $current_version"
    exit 1
}

TEMP=`getopt -o 'gpfu:P:' -l 'get-version,push,force-version,update-version:packager:' -- "$@"`

if [ $? != 0 ] ; then usage >&2 ; fi

eval set -- "$TEMP"

new_version=
gver=0
push=0
packager=wallix-packager
force_vers=0
while true; do
  case "$1" in
    -g|--get-version) gver=1; shift ;;
    -p|--push) push=1; shift ;;
    -f|--force-version) force_vers=1; shift ;;
    -u|--update-version)
        [ -z "$2" ] && usage
        new_version="$2"
        shift 2
        ;;
    -P|--packager)
        packager="$2"
        [ -d "$2" ] && packager+="/packager.py"
        shift 2
        ;;
    --) shift; break ;;
    * ) usage; exit 1 ;;
  esac
done


if [ $gver = 1 ] ; then
    echo "$current_version"
    exit
fi


if [ -z "$new_version" ] ; then
    echo missing --new-version
    usage
fi

if [ $force_vers = 0 ] && [ "$current_version" = "$new_version" ] ; then
    echo version is already to "$new_version"
    exit 2
fi

gdiff=$(GIT_PAGER=cat git diff --shortstat)

if [ $? != 0 ] || [ "$gdiff" != '' ] ; then
    echo -e "your repository has uncommited changes:\n$gdiff\nPlease commit before packaging." >&2
    exit 2
fi

check_tag ()
{
    while read l ; do
        if [ "$l" = "$new_version" ] ; then
            echo "tag $new_version already exists ("$1")."
            exit 2
        fi
    done
}

git tag --list | check_tag locale
git ls-remote --tags origin | sed 's#.*/##' | check_tag remote

echo -e "#pragma once\n#define RVT_LIB_VERSION \"$new_version\"" > "$version_file"

$packager --version "$new_version" "$@" && \
git commit -am "Version $new_version" && git tag "$new_version" && {
    [ $push = 1 ] && git push && git push --tags
}

exit $?
