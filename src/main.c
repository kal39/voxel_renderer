#include <stdio.h>

#include "logger/logger.h"
#include "lua/lua_extra.h"
#include "renderer/renderer.h"
#include "stb/stb_image_write.h"

typedef struct LogArg {
    lua_State* l;
    int logFunction;
} LogArg;

static void l_log(
    void* arg,
    int lvl,
    const char* src,
    const char* file,
    int line,
    const char* msg
) {
    lua_State* l = ((LogArg*)arg)->l;
    int logFunction = ((LogArg*)arg)->logFunction;

    lua_rawgeti(l, LUA_REGISTRYINDEX, logFunction);
    lua_push_f(l, "i; s; s; i; s", lvl, src, file, line, msg);
    if (lua_pcall(l, 5, 0, 0)) {
        printf("error in log function: %s\n", lua_tostring(l, -1));
    }
}

static int l_scene_register_material(lua_State* l) {
    Scene* scene;
    vec3 color;
    float emission;
    bool res = lua_pop_f(
        l,
        "{color: {1: f, 2: f, 3: f}, emission: f}; {_scene: u}",
        &color.r,
        &color.g,
        &color.b,
        &emission,
        &scene
    );

    if (!res) lua_raise_error(l, "invalid material");
    Material m = material(color, emission);
    lua_pushinteger(l, scene_register_material(scene, m));
    return 1;
}

static int l_scene_set(lua_State* l) {
    Scene* scene;
    vec3 pos;
    int materialID;
    bool res = lua_pop_f(
        l,
        "i; {1: f, 2: f, 3: f}; {_scene: u}",
        &materialID,
        &pos.x,
        &pos.y,
        &pos.z,
        &scene
    );

    if (!res) lua_raise_error(l, "invalid position or material");
    scene_set(
        scene,
        (uvec3){.x = (int)pos.x, .y = (int)pos.y, .z = (int)pos.z},
        materialID
    );
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        ERROR("usage: %s <config file>", argv[0]);
        return 1;
    }

    char* fileName = argv[1];

    lua_State* l = luaL_newstate();
    luaL_openlibs(l);

    INFO("reading config file \"%s\n\"", fileName);
    if (luaL_dofile(l, fileName)) {
        ERROR(
            "failed to run config file \"%s\": %s\n",
            fileName,
            lua_tostring(l, -1)
        );
        return 1;
    }

    char* outputFile;
    int logFunction, deviceFunction, sceneDataFunction;
    RenderSettings rendererSettings;
    SceneCreateInfo sceneCreateInfo;
    CameraCreateInfo cameraCreateInfo;

    char* format = "{"
                   "    output_file: s,"
                   "    logger: l,"
                   "    device_selector: l,"
                   "    renderer: {"
                   "        renderer_code: s,"
                   "        iteration_code: s,"
                   "        output_code: s,"
                   "        workgroup_size: {1: i, 2: i},"
                   "        image_size: {1: i, 2: i},"
                   "        iterations: i,"
                   "        max_depth: i"
                   "    },"
                   "    scene: {"
                   "        size: {1: i, 2: i, 3: i},"
                   "        bg: {color: {1: f, 2: f, 3: f}, emission: f},"
                   "        voxel_placer: l"
                   "    },"
                   "    camera: {"
                   "        sensor_size: {1: f, 2: f},"
                   "        focal_length: f,"
                   "        position: {1: f, 2: f, 3: f},"
                   "        rotation: {1: f, 2: f, 3: f}"
                   "    }"
                   "}";

    bool res = lua_pop_f(
        l,
        format,
        &outputFile,
        &logFunction,
        &deviceFunction,
        &rendererSettings.rendererCode,
        &rendererSettings.iterationCode,
        &rendererSettings.outputCode,
        &rendererSettings.wgSize.x,
        &rendererSettings.wgSize.y,
        &rendererSettings.imageSize.x,
        &rendererSettings.imageSize.y,
        &rendererSettings.iterations,
        &rendererSettings.maxRayDepth,
        &sceneCreateInfo.size.x,
        &sceneCreateInfo.size.y,
        &sceneCreateInfo.size.z,
        &sceneCreateInfo.bg.color.r,
        &sceneCreateInfo.bg.color.g,
        &sceneCreateInfo.bg.color.b,
        &sceneCreateInfo.bg.properties.x,
        &sceneDataFunction,
        &cameraCreateInfo.sensorSize.x,
        &cameraCreateInfo.sensorSize.y,
        &cameraCreateInfo.focalLength,
        &cameraCreateInfo.pos.x,
        &cameraCreateInfo.pos.y,
        &cameraCreateInfo.pos.z,
        &cameraCreateInfo.rot.x,
        &cameraCreateInfo.rot.y,
        &cameraCreateInfo.rot.z
    );

    if (!res) {
        ERROR("failed to parse config file \"%s\"\n", fileName);
        return 1;
    }

    LogArg logArg = {l, logFunction};
    set_log_fn(l_log, &logArg);

    INFO("creating microcompute instance");
    mc_Instance* instance = mc_instance_create((mc_log_fn*)new_log, NULL);
    if (instance == NULL) {
        ERROR("failed to create microcompute instance");
        return 1;
    }

    INFO("running device selection function\n");
    lua_rawgeti(l, LUA_REGISTRYINDEX, deviceFunction);
    int deviceCount = (int)mc_instance_get_device_count(instance);

    lua_newtable(l);
    for (int i = 0; i < deviceCount; i++) {
        mc_Device* dev = mc_instance_get_devices(instance)[i];
        char* name = mc_device_get_name(dev);
        char* type = (char*)mc_device_type_to_str(mc_device_get_type(dev)) + 15;
        lua_push_f(l, "i; {name: s, type: s}", i + 1, name, type);
        lua_settable(l, -3);
    }

    if (lua_pcall(l, 1, 1, 0)) {
        ERROR("error in device selector function: %s\n", lua_tostring(l, -1));
        return 1;
    }

    int deviceIndex;
    res = lua_pop_f(l, "i", &deviceIndex);

    if (!res || deviceIndex < 1 || deviceIndex > deviceCount) {
        ERROR("invalid device index");
        return 1;
    }

    mc_Device* dev = mc_instance_get_devices(instance)[deviceIndex - 1];

    INFO("using device \"%s\"", mc_device_get_name(dev));

    Scene* scene = scene_create(dev, sceneCreateInfo);
    if (scene == NULL) {
        ERROR("failed to create scene");
        return 1;
    }

    Camera* camera = camera_create(dev, cameraCreateInfo);
    if (camera == NULL) {
        ERROR("failed to create camera");
        return 1;
    }

    INFO("running voxel placer function\n");
    lua_rawgeti(l, LUA_REGISTRYINDEX, sceneDataFunction);

    lua_push_f(
        l,
        "{_scene: u, size: {1: i, 2: i, 3: i}, register_material: l, set: l}",
        scene,
        sceneCreateInfo.size.x,
        sceneCreateInfo.size.y,
        sceneCreateInfo.size.z,
        l_scene_register_material,
        l_scene_set
    );

    if (lua_pcall(l, 1, 0, 0)) {
        ERROR("error in voxel placer function: %s\n", lua_tostring(l, -1));
        return 1;
    }

    unsigned char* image = render(dev, rendererSettings, scene, camera);
    if (image == NULL) {
        ERROR("failed to render image");
        return 1;
    }

    INFO("writing image to \"%s\"", outputFile);
    stbi_write_bmp(
        outputFile,
        (int)rendererSettings.imageSize.x,
        (int)rendererSettings.imageSize.y,
        4,
        image
    );

    INFO("cleanup");
    free(image);
    scene_destroy(scene);
    camera_destroy(camera);
    mc_instance_destroy(instance);

    INFO("all done, goodbye!");

    lua_close(l);
    return 0;
}