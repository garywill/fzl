#! /bin/sh

makerelease()
{
  local RELEASEDIR="/home/nightlybuild/releases"

  rm -rf "$RELEASEDIR" > /dev/null 2>&1
  mkdir -p "$RELEASEDIR"

  local CONFIGUREIN="$WORKDIR/source/FileZilla3/configure.ac"

  local version=

  echo "Creating release files"

  exec 4<"$CONFIGUREIN" || return 1
  read <&4 -r version || return 1

  version="${version#*, }"
  version="${version%,*}"

  echo "Version: $version"

  cd "$OUTPUTDIR"

  cp "FileZilla3-src.tar.bz2" "$RELEASEDIR/FileZilla_${version}_src.tar.bz2"

  for TARGET in *; do
    if ! [ -f "$OUTPUTDIR/$TARGET/build.log" ]; then
      continue;
    fi
    if ! [ -f "$OUTPUTDIR/$TARGET/successful" ]; then
      continue;
    fi

    cd "$OUTPUTDIR/$TARGET"
    for i in FileZilla*; do
      local lext=${i#*.}
      local sext=${i##*.}

      if [ "$sext" = "sha512" ]; then
        continue
      fi

      locale platform=
      case "$TARGET" in
        *mingw*)
          platform=win32
	  if [ "$lext" = "exe" ]; then
	    platform="${platform}-setup"
	  fi
          ;;
        *)
          platform="$TARGET"
          ;;
      esac

      local name="FileZilla_${version}_${platform}.$lext"
      echo $name

      cp "$i" "${RELEASEDIR}/${name}"
    done
  done

  cd ${RELEASEDIR}

  sha512sum --binary * > "FileZilla_${version}.sha512"
}

makerelease
