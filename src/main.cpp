#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <set>
#include <algorithm>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Instrucciones que recibe la IA en cada peticion
// ---------------------------------------------------------------------------
static const char* SYSTEM_PROMPT = R"PROMPT(
Eres un asistente que construye niveles de Geometry Dash dentro del editor y habla con el jugador en espanol.
Respondes SIEMPRE con un unico objeto JSON valido, sin texto fuera de el y sin bloques de codigo:
{"reply":"mensaje corto para el chat","actions":[...],"look":{"from":0,"to":60}}

- reply: 1 a 3 frases.
- actions (opcional): lista de acciones. Tipos:
  {"type":"add","id":8,"gx":14,"gy":0,"rot":0,"scale":1}
  {"type":"fill","id":1,"x1":49,"y1":0,"x2":53,"y2":2}   (rectangulo relleno de objetos)
  {"type":"delete_area","x1":0,"y1":0,"x2":10,"y2":5}      (solo funciona si el jugador lo permite)
- look (opcional): pide ver el mapa de esas columnas para revisar tu trabajo. Usalo despues de construir algo importante.

COORDENADAS: gx es la columna (1 bloque = 1), gy la fila. gy 0 = objeto apoyado en el suelo. Todo va en enteros.
Portales (12,13,47,111,660) y portales de velocidad: usa gy 1. Pads y picos del suelo: gy 0. Picos del techo: rot 180.

IDS PERMITIDOS: bloques 1-7, 40 (losa); picos 8, 39, 103; pads 35 (amarillo), 67 (azul), 140 (rosa), 1332 (rojo);
orbes 36 (amarillo), 84 (azul), 141 (rosa), 1022 (verde), 1330 (negro), 1333 (rojo);
portales 12 (cubo), 13 (nave), 47 (bola), 111 (ufo), 660 (wave), 10 (gravedad normal), 11 (gravedad invertida),
45 (espejo), 46, 99, 101 (mini); velocidad 200 (0.7x), 201 (1x), 202 (1.24x), 203 (1.5x), 1334 (1.85x); moneda 142.

REGLAS PARA QUE EL NIVEL SEA JUGABLE (velocidad 1x):
- Solo picos simples o dobles. Nunca triples.
- Entre un grupo de picos y el siguiente deja al menos 6 bloques libres (8 con velocidad 202 o mayor).
- Despues de un pad deja 12 bloques libres sin peligros.
- Un cubo salta como maximo 2 bloques de alto con margen justo: sube escaleras de 1 bloque cada vez, con 5 columnas de ancho por escalon.
- Pasillos de nave: separacion de al menos 6 bloques entre suelo y techo y cambios de 1 bloque cada 4 columnas o mas.
- Seccion de bola: pasillo cerrado por bloques arriba y abajo, picos alternados suelo/techo separados 10 columnas, y un numero par de picos para terminar en el suelo.
- No pongas nada solido a menos de 6 columnas despues de un portal.
- No uses mas de 1500 objetos por respuesta.

RECIBES en cada mensaje [ESTADO DEL EDITOR] con un mapa ASCII (1 caracter = 1 bloque, fila 0 abajo) y el resultado de tu ultima accion.
Usalo para ver donde construir y para comprobar tu trabajo. Si el jugador solo conversa, responde sin actions.
)PROMPT";

// ---------------------------------------------------------------------------
// Estado compartido entre aperturas del menu
// ---------------------------------------------------------------------------
static std::vector<std::pair<std::string, std::string>> g_history;
static std::string g_chat;
static std::string g_lastResult;
static std::vector<GameObject*> g_lastBatch;

static std::string trimKey(std::string s) {
    auto bad = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '\''; };
    while (!s.empty() && bad(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && bad(s[i])) ++i;
    return s.substr(i);
}

// ---------------------------------------------------------------------------
// "Ojos" de la IA: convierte el editor en un mapa de texto
// ---------------------------------------------------------------------------
static char symbolFor(GameObject* o) {
    int id = o->m_objectID;
    if (id >= 1 && id <= 7) return 'B';
    if (id == 40) return 'B';
    if (id == 8 || id == 39 || id == 103) {
        int r = (static_cast<int>(o->getRotation()) % 360 + 360) % 360;
        return r == 180 ? 'v' : '^';
    }
    if (id == 35 || id == 67 || id == 140 || id == 1332) return 'p';
    if (id == 36 || id == 84 || id == 141 || id == 1022 || id == 1330 || id == 1333) return 'o';
    if (id == 12 || id == 13 || id == 47 || id == 111 || id == 660 || id == 10 || id == 11 ||
        id == 45 || id == 46 || id == 99 || id == 101) return 'P';
    if (id == 200 || id == 201 || id == 202 || id == 203 || id == 1334) return 'S';
    if (id == 142) return 'C';
    if (id == 899 || id == 3600) return 'T';
    return '#';
}

static int rankOf(char c) {
    switch (c) {
        case '.': return 0;
        case '#': return 1;
        case 'B': return 2;
        case 'T': return 3;
        case 'C': return 4;
        case 'p': case 'o': return 5;
        case 'S': case 'P': return 6;
        case '^': case 'v': return 7;
    }
    return 1;
}

static std::string describeLevel(LevelEditorLayer* lel, int from, int to) {
    CCArray* objs = lel->m_objects;
    int total = objs ? objs->count() : 0;

    if (from == 0 && to == 0) {
        float scale = lel->m_objectLayer->getScale();
        if (scale <= 0.f) scale = 1.f;
        float left = -lel->m_objectLayer->getPositionX() / scale;
        from = std::max(0, static_cast<int>(std::floor(left / 30.f)));
        to = from + 70;
    }
    if (to < from) std::swap(from, to);
    to = std::min(to, from + 119);

    const int rows = 14;
    std::vector<std::string> grid(rows, std::string(to - from + 1, '.'));
    int maxGx = 0;
    for (int i = 0; i < total; ++i) {
        auto o = static_cast<GameObject*>(objs->objectAtIndex(i));
        if (!o) continue;
        int gx = static_cast<int>(std::floor(o->getPositionX() / 30.f));
        int gy = static_cast<int>(std::floor(o->getPositionY() / 30.f));
        maxGx = std::max(maxGx, gx);
        if (gx < from || gx > to || gy < 0 || gy >= rows) continue;
        char c = symbolFor(o);
        char& cell = grid[gy][gx - from];
        if (rankOf(c) >= rankOf(cell)) cell = c;
    }

    std::string out = fmt::format(
        "Objetos totales: {}. Columna maxima ocupada: {}.\n"
        "Mapa columnas {}..{} (B bloque, ^ pico, v pico de techo, p pad, o orbe, P portal, S velocidad, C moneda, T trigger, # otro):\n",
        total, maxGx, from, to);
    for (int r = rows - 1; r >= 0; --r) {
        out += fmt::format("{:2}|{}\n", r, grid[r]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Menu de chat
// ---------------------------------------------------------------------------
class AIChatPopup : public Popup {
protected:
    EditorUI* m_ui = nullptr;
    TextInput* m_input = nullptr;
    CCLabelBMFont* m_log = nullptr;
    CCLabelBMFont* m_status = nullptr;
    async::TaskHolder<web::WebResponse> m_listener;
    bool m_busy = false;
    float m_logMaxHeight = 150.f;
    int m_created = 0;
    int m_deleted = 0;
    int m_errors = 0;

    bool init(EditorUI* ui) {
        if (!Popup::init(430.f, 290.f)) return false;
        m_ui = ui;
        this->setTitle("IA Editor v0.2");

        auto size = m_mainLayer->getContentSize();
        float boxW = size.width - 30.f;
        float boxH = size.height - 120.f;
        m_logMaxHeight = boxH - 12.f;

        auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        bg->setColor({0, 0, 0});
        bg->setOpacity(160);
        bg->setContentSize({boxW, boxH});
        bg->setPosition({size.width / 2.f, size.height - 42.f - boxH / 2.f});
        m_mainLayer->addChild(bg);

        const float logScale = 0.55f;
        m_log = CCLabelBMFont::create("", "chatFont.fnt", (boxW - 16.f) / logScale, kCCTextAlignmentLeft);
        m_log->setScale(logScale);
        m_log->setAnchorPoint({0.f, 1.f});
        m_log->setPosition({15.f + 8.f, size.height - 42.f - 6.f});
        m_mainLayer->addChild(m_log);

        m_status = CCLabelBMFont::create("", "chatFont.fnt");
        m_status->setScale(0.5f);
        m_status->setPosition({size.width / 2.f, 62.f});
        m_mainLayer->addChild(m_status);

        m_input = TextInput::create(size.width - 150.f, "Dime que construir...", "chatFont.fnt");
        m_input->setPosition({size.width / 2.f - 55.f, 38.f});
        m_mainLayer->addChild(m_input);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(menu);

        auto sendSpr = ButtonSprite::create("Enviar", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        auto sendBtn = CCMenuItemSpriteExtra::create(sendSpr, this, menu_selector(AIChatPopup::onSend));
        sendBtn->setPosition({size.width - 55.f, 38.f});
        menu->addChild(sendBtn);

        auto undoSpr = ButtonSprite::create("Deshacer IA", "goldFont.fnt", "GJ_button_06.png", 0.5f);
        auto undoBtn = CCMenuItemSpriteExtra::create(undoSpr, this, menu_selector(AIChatPopup::onUndo));
        undoBtn->setPosition({70.f, 12.f});
        menu->addChild(undoBtn);

        if (g_chat.empty()) {
            g_chat = "IA: [v0.2] Hola, soy tu ayudante de editor. Dime que quieres construir. Para guardar tu key escribe: /clave TU_KEY\n";
        }
        this->refreshLog();
        return true;
    }

    void refreshLog() {
        std::string text = g_chat;
        for (int guard = 0; guard < 300; ++guard) {
            m_log->setString(text.c_str());
            if (m_log->getContentSize().height * m_log->getScale() <= m_logMaxHeight) break;
            auto pos = text.find('\n');
            if (pos == std::string::npos || pos + 1 >= text.size()) break;
            text.erase(0, pos + 1);
        }
        g_chat = text;
    }

    void appendChat(std::string const& line) {
        g_chat += line + "\n";
        this->refreshLog();
    }

    void setStatus(std::string const& s) { m_status->setString(s.c_str()); }

    void onSend(CCObject*) {
        if (m_busy) return;
        std::string text(m_input->getString());
        if (text.empty()) return;
        m_input->setString("");
        // /clave TU_API_KEY : guarda la clave sin pasar por los ajustes
        if (text.rfind("/clave", 0) == 0) {
            std::string key = trimKey(text.size() > 6 ? text.substr(6) : "");
            if (key.empty()) {
                this->appendChat("Sistema: escribe /clave seguido de tu API key.");
            } else {
                (void)Mod::get()->setSettingValue<std::string>("api-key", key);
                this->appendChat(fmt::format("Sistema: clave guardada ({} caracteres, empieza con '{}').",
                                             key.size(), key.substr(0, 3)));
            }
            return;
        }
        this->appendChat("Tu: " + text);
        this->ask(text, 0, 0, 0);
    }

    void onUndo(CCObject*) {
        auto lel = m_ui->m_editorLayer;
        int n = 0;
        for (auto o : g_lastBatch) {
            if (lel->m_objects && lel->m_objects->containsObject(o)) {
                lel->removeObject(o, true);
                ++n;
            }
        }
        g_lastBatch.clear();
        g_lastResult = fmt::format("El jugador deshizo tu ultimo lote ({} objetos quitados).", n);
        this->appendChat(fmt::format("Sistema: deshechos {} objetos de la IA.", n));
    }

    // -------- peticion a Gemini --------
    void ask(std::string userText, int depth, int lookFrom, int lookTo) {
        auto key = trimKey(Mod::get()->getSettingValue<std::string>("api-key"));
        if (key.empty()) {
            this->appendChat("Sistema: falta la API key. Ponla en los ajustes del mod.");
            return;
        }
        auto model = Mod::get()->getSettingValue<std::string>("model");
        if (model.empty()) model = "gemini-flash-latest";

        m_busy = true;
        this->setStatus("Pensando...");

        std::string payload = userText;
        if (!g_lastResult.empty()) payload += "\n\n[ULTIMO RESULTADO]\n" + g_lastResult;
        payload += "\n\n[ESTADO DEL EDITOR]\n" + describeLevel(m_ui->m_editorLayer, lookFrom, lookTo);
        g_history.push_back({"user", payload});
        while (g_history.size() > 10) g_history.erase(g_history.begin());
        while (!g_history.empty() && g_history.front().first != "user") g_history.erase(g_history.begin());

        matjson::Value body = matjson::Value::object();

        matjson::Value sysParts = matjson::Value::array();
        matjson::Value sysPart = matjson::Value::object();
        sysPart["text"] = std::string(SYSTEM_PROMPT);
        sysParts.push(sysPart);
        matjson::Value sys = matjson::Value::object();
        sys["parts"] = sysParts;
        body["systemInstruction"] = sys;

        matjson::Value contents = matjson::Value::array();
        for (auto const& turn : g_history) {
            matjson::Value part = matjson::Value::object();
            part["text"] = turn.second;
            matjson::Value parts = matjson::Value::array();
            parts.push(part);
            matjson::Value item = matjson::Value::object();
            item["role"] = turn.first;
            item["parts"] = parts;
            contents.push(item);
        }
        body["contents"] = contents;

        matjson::Value cfg = matjson::Value::object();
        cfg["responseMimeType"] = std::string("application/json");
        cfg["temperature"] = 0.4;
        body["generationConfig"] = cfg;

        web::WebRequest req;
        req.header("Content-Type", "application/json");
        req.header("x-goog-api-key", key);
        req.bodyJSON(body);

        auto url = fmt::format(
            "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent", model);

        m_listener.spawn(req.post(url), [this, depth](web::WebResponse res) {
            this->onResponse(res, depth);
        });
    }

    void onResponse(web::WebResponse& res, int depth) {
        m_busy = false;
        this->setStatus("");

        if (!res.ok()) {
            if (!g_history.empty()) g_history.pop_back();
            std::string raw = res.string().unwrapOr("");
            if (raw.size() > 160) raw = raw.substr(0, 160);
            this->appendChat(fmt::format("Sistema: error {} de la API. {}", res.code(), raw));
            if (res.code() == 400) {
                auto k = trimKey(Mod::get()->getSettingValue<std::string>("api-key"));
                this->appendChat(fmt::format("Sistema: la clave guardada tiene {} caracteres y empieza con '{}'.",
                                             k.size(), k.substr(0, 3)));
            }
            return;
        }

        auto json = res.json();
        if (!json) {
            if (!g_history.empty()) g_history.pop_back();
            this->appendChat("Sistema: no pude leer la respuesta de la API.");
            return;
        }
        auto root = json.unwrap();
        std::string text = root["candidates"][static_cast<size_t>(0)]["content"]["parts"][static_cast<size_t>(0)]["text"]
                               .asString().unwrapOr("");
        if (text.empty()) {
            if (!g_history.empty()) g_history.pop_back();
            this->appendChat("Sistema: la IA no devolvio texto (puede haber sido bloqueada).");
            return;
        }
        g_history.push_back({"model", text});

        // quitar posibles vallas ``` y quedarnos con el JSON
        auto a = text.find('{');
        auto b = text.rfind('}');
        std::string jsonText = (a != std::string::npos && b != std::string::npos && b > a)
                                   ? text.substr(a, b - a + 1) : text;

        auto parsed = matjson::parse(jsonText);
        if (!parsed) {
            this->appendChat("IA: " + text.substr(0, 300));
            return;
        }
        auto data = parsed.unwrap();

        std::string reply = data["reply"].asString().unwrapOr("");
        m_created = m_deleted = m_errors = 0;
        std::vector<GameObject*> batch;

        if (data.contains("actions")) {
            auto arr = data["actions"].asArray();
            if (arr) {
                for (auto& act : arr.unwrap()) {
                    this->runAction(act, batch);
                }
            }
        }
        if (!batch.empty()) g_lastBatch = batch;

        g_lastResult = fmt::format("Creados {} objetos, borrados {}, acciones con error {}.",
                                   m_created, m_deleted, m_errors);
        if (reply.empty()) reply = "Hecho.";
        this->appendChat("IA: " + reply);
        if (m_created || m_deleted || m_errors) {
            this->appendChat("Sistema: " + g_lastResult);
        }

        // la IA pide mirar el resultado: le mandamos el mapa y deja que corrija
        if (data.contains("look") && depth < 2) {
            int from = static_cast<int>(data["look"]["from"].asInt().unwrapOr(0));
            int to = static_cast<int>(data["look"]["to"].asInt().unwrapOr(0));
            this->ask("[Sistema] Aqui tienes el mapa que pediste. Revisa si quedo bien y corrige solo si hace falta.",
                      depth + 1, from, to);
        }
    }

    // -------- acciones sobre el editor --------
    static bool idAllowed(int id) {
        static const std::set<int> ok = {
            1, 2, 3, 4, 5, 6, 7, 40, 8, 39, 103,
            35, 67, 140, 1332, 36, 84, 141, 1022, 1330, 1333,
            10, 11, 12, 13, 45, 46, 47, 99, 101, 111, 660,
            200, 201, 202, 203, 1334, 142
        };
        return ok.count(id) > 0;
    }

    bool place(int id, double gx, double gy, int rot, float scale, std::vector<GameObject*>& batch) {
        if (!idAllowed(id) || m_created >= 1500) return false;
        if (gx < 0 || gx > 5000 || gy < 0 || gy > 40) return false;
        auto lel = m_ui->m_editorLayer;
        auto obj = lel->createObject(id, ccp(gx * 30.0 + 15.0, gy * 30.0 + 15.0), false);
        if (!obj) return false;
        if (rot) obj->setRotation(static_cast<float>(rot));
        if (scale > 0.f && scale != 1.f) obj->setScale(scale);
        batch.push_back(obj);
        ++m_created;
        return true;
    }

    void runAction(matjson::Value& act, std::vector<GameObject*>& batch) {
        std::string type = act["type"].asString().unwrapOr("");
        if (type == "add") {
            int id = static_cast<int>(act["id"].asInt().unwrapOr(0));
            double gx = act["gx"].asDouble().unwrapOr(-1);
            double gy = act["gy"].asDouble().unwrapOr(-1);
            int rot = static_cast<int>(act["rot"].asInt().unwrapOr(0));
            float scale = static_cast<float>(act["scale"].asDouble().unwrapOr(1.0));
            if (!this->place(id, gx, gy, rot, scale, batch)) ++m_errors;
        } else if (type == "fill") {
            int id = static_cast<int>(act["id"].asInt().unwrapOr(0));
            int x1 = static_cast<int>(act["x1"].asInt().unwrapOr(0));
            int y1 = static_cast<int>(act["y1"].asInt().unwrapOr(0));
            int x2 = static_cast<int>(act["x2"].asInt().unwrapOr(0));
            int y2 = static_cast<int>(act["y2"].asInt().unwrapOr(0));
            if (x2 < x1) std::swap(x1, x2);
            if (y2 < y1) std::swap(y1, y2);
            if ((x2 - x1 + 1) * (y2 - y1 + 1) > 600) { ++m_errors; return; }
            for (int x = x1; x <= x2; ++x)
                for (int y = y1; y <= y2; ++y)
                    if (!this->place(id, x, y, 0, 1.f, batch)) ++m_errors;
        } else if (type == "delete_area") {
            if (!Mod::get()->getSettingValue<bool>("allow-delete")) { ++m_errors; return; }
            int x1 = static_cast<int>(act["x1"].asInt().unwrapOr(0));
            int y1 = static_cast<int>(act["y1"].asInt().unwrapOr(0));
            int x2 = static_cast<int>(act["x2"].asInt().unwrapOr(0));
            int y2 = static_cast<int>(act["y2"].asInt().unwrapOr(0));
            if (x2 < x1) std::swap(x1, x2);
            if (y2 < y1) std::swap(y1, y2);
            auto lel = m_ui->m_editorLayer;
            std::vector<GameObject*> toRemove;
            CCArray* objs = lel->m_objects;
            for (int i = 0; objs && i < objs->count(); ++i) {
                auto o = static_cast<GameObject*>(objs->objectAtIndex(i));
                if (!o) continue;
                int gx = static_cast<int>(std::floor(o->getPositionX() / 30.f));
                int gy = static_cast<int>(std::floor(o->getPositionY() / 30.f));
                if (gx >= x1 && gx <= x2 && gy >= y1 && gy <= y2) toRemove.push_back(o);
            }
            for (auto o : toRemove) { lel->removeObject(o, true); ++m_deleted; }
        } else {
            ++m_errors;
        }
    }

public:
    static AIChatPopup* create(EditorUI* ui) {
        auto ret = new AIChatPopup();
        if (ret->init(ui)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Boton negro en el editor
// ---------------------------------------------------------------------------
class $modify(AIEditorUI, EditorUI) {
    bool init(LevelEditorLayer* lel) {
        if (!EditorUI::init(lel)) return false;

        auto dot = CCDrawNode::create();
        dot->drawDot({20.f, 20.f}, 20.f, {0.f, 0.f, 0.f, 1.f});
        dot->setContentSize({40.f, 40.f});
        auto label = CCLabelBMFont::create("IA", "bigFont.fnt");
        label->setScale(0.55f);
        label->setPosition({20.f, 20.f});
        dot->addChild(label);

        auto btn = CCMenuItemSpriteExtra::create(dot, this, menu_selector(AIEditorUI::onAIButton));
        btn->setID("ia-editor-button"_spr);

        auto menu = CCMenu::create();
        menu->setID("ia-editor-menu"_spr);
        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 30.f, win.height / 2.f + 70.f});
        menu->addChild(btn);
        this->addChild(menu, 100);
        return true;
    }

    void onAIButton(CCObject*) {
        if (auto p = AIChatPopup::create(this)) p->show();
    }
};
