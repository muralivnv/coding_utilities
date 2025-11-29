# constants
docker_image_name := "coding_utilities"

build_img:
    docker build -t {{docker_image_name}} .

launch_container:
    docker run --name {{docker_image_name}} -it --rm --net=host --privileged \
    -e WAYLAND_DISPLAY=$WAYLAND_DISPLAY -e XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR \
    -v $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY:$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY \
    --device=/dev/dri:/dev/dri -v $HOME:$HOME -w $(pwd) \
    {{docker_image_name}}
