#include "PythonUtils.h"
#include "MinecraftUpdateHook.h"

void PythonApi::executeCommand(const std::string& cmd) {
    std::string code(
            "import msgpack, _pynetmodule\n"
            "import mod.client.extraClientApi as clientApi\n"
            "from gui_2d import GUI\n"
            "\n"
            "def send_pyrpc(namespace, system, event, data):\n"
            "    pkt = msgpack.packb(('ModEventC2S', (namespace, system, event, data), None), use_bin_type=True, strict_types=True, default=lambda o: {'__type__': 'tuple', 'value': list(o)} if isinstance(o, tuple) else o)\n"
            "    _pynetmodule.send2server(98247598, pkt, len(pkt))\n"
            "\n"
            "send_pyrpc('Minecraft', 'aiCommand', 'ExecuteCommandEvent', {\n"
            "    'playerId': clientApi.GetLocalPlayerId(),\n"
            "    'cmd': '" + cmd + "',\n"
                                   "    'uuid': '0',\n"
                                   "    'aiModel': '-1'\n"
                                   "})\n"
    );
    PythonUtils::PyExec(code, true);
}

void PythonApi::setCameraAnchor(Vec3 anchor, bool imm) {
    std::string str = std::to_string(anchor.x) + ", " + std::to_string(anchor.y) + ", " + std::to_string(anchor.z);
    std::string msg("import mod.client.extraClientApi as clientApi\n"
                    "comp = clientApi.GetEngineCompFactory().CreateCamera(clientApi.GetLevelId())\n"
                    "comp.SetCameraAnchor((" + str + "))");
    if (imm) {
        PythonUtils::PyExec(msg, true);
    }
}

void PythonApi::setPickRange(float range) {
    std::string code("import minecraft.localplayermodule\n"
                     "localplayermodule.setPickRange(" + std::to_string(range) + ")");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setEnableGaussianBlur(bool value) {
    std::string enable = value ? "True" : "False";
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetEnableGaussianBlur(" + enable + ")");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setGaussianBlurRadius(float radius) {
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetGaussianBlurRadius(" + std::to_string(radius) + ")");
    if (radius < 10) {
        PythonUtils::PyExec(code, true);
    }
}

void PythonApi::setEnableVignette(bool value) {
    std::string enable = value ? "True" : "False";
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetEnableVignette(" + enable + ")");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setVignetteCenter(float x, float y) {
    std::string str = std::to_string(x) + "," + std::to_string(y);
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetVignetteCenter((" + str + "))");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setVignetteRGB(float r, float g, float b) {
    std::string str = std::to_string(r) + "," + std::to_string(g) + "," +std::to_string(b);
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetVignetteRGB((" + str + "))");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setVignetteRadius(float radius) {
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetVignetteRadius(" + std::to_string(radius) + ")");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setVignetteSmoothness(float radius) {
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetVignetteSmoothness(" + std::to_string(radius) + ")");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setEnableColorAdjustment(bool value) {
    std::string enable = value ? "True" : "False";
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetEnableColorAdjustment(" + enable + ")");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setColorAdjustmentBrightness(float brightness) {
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetColorAdjustmentBrightness(" + std::to_string(brightness) + ")");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setColorAdjustmentContrast(float contrast) {
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetColorAdjustmentContrast(" + std::to_string(contrast) + ")");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setColorAdjustmentSaturation(float saturation) {
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetColorAdjustmentSaturation(" + std::to_string(saturation) + ")");
    PythonUtils::PyExec(code, true);
}

void PythonApi::setColorAdjustmentTint(float intensity, float r, float g, float b) {
    std::string str = std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b);
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreatePostProcess(clientApi.GetLevelId())\n"
                     "comp.SetColorAdjustmentTint(" + std::to_string(intensity) + ",(" + str + "))");
    PythonUtils::PyExec(code, true);
}

void PythonApi::HideNameTag(bool value) {
    std::string enable = value ? "True" : "False";
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "clientApi.HideNameTag(" +enable+ ")");
    PythonUtils::PyExec(code, true);
}

float PythonApi::getFPS() {
    float fps = 0;
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreateGame(clientApi.GetLevelId())\n"
                     "fps = comp.GetFps()");
    PythonUtils::PyExec(code, true);
    return fps;
}

void PythonApi::setSkyColor(float r, float g, float b, float a) {
    std::string str = std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) + "," + std::to_string(a);
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreateSkyRender(clientApi.GetLevelId())\n"
                     "comp.SetSkyColor((" + str + "))");
    PythonUtils::PyExec(code, true);
}

void PythonApi::ResetSkyColor() {
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreateSkyRender(clientApi.GetLevelId())\n"
                     "comp.ResetSkyColor()");
    PythonUtils::PyExec(code, true);
}

void PythonApi::show_toast(std::string text){
}

void PythonApi::setDeBug() {
    std::string code("import mod.client.extraClientApi as clientApi\n"
                     "comp = clientApi.GetEngineCompFactory().CreateFog(clientApi.GetLevelId())\n"
                     "comp.SetFogColor((1.0,1.0,1.0,1.0))");
    PythonUtils::PyExec(code, true);
}

void PythonApi::summonVehicle() {
    std::string code(
        "import msgpack, _pynetmodule\n"
        "import mod.client.extraClientApi as clientApi\n"
        "\n"
        "playerId = str(clientApi.GetLocalPlayerId())\n"
        "\n"
        "data1 = msgpack.packb(('r', (8, 'Minecraft:pet:teleport_mount_request'), None), use_bin_type=True)\n"
        "data2 = msgpack.packb(('c', (8, {'playerId': playerId}), None), use_bin_type=True)\n"
        "\n"
        "_pynetmodule.send2server(98247598, data1, len(data1))\n"
        "_pynetmodule.send2server(98247598, data2, len(data2))\n"
    );
    SubmitPythonCode(code);
}

