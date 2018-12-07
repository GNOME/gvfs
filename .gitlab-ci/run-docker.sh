#!/bin/bash

TAG="registry.gitlab.gnome.org/gnome/gvfs/master:v3"

if [[ "$1" == "--no-cache" ]]; then
  NOCACHE="--no-cache"
fi

docker build $NOCACHE -t $TAG .

if [[ "$1" == "--push" ]]; then
  docker login registry.gitlab.gnome.org
  docker push $TAG
fi