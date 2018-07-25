#!/bin/bash

set -ve

TAG="registry.gitlab.gnome.org/gnome/gvfs/master:v2"

docker build -t $TAG .

if [[ "$1" == "--push" ]]; then
  docker login registry.gitlab.gnome.org
  docker push $TAG
fi