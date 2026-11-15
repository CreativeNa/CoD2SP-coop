"""make_coop_str.py - the texts of the CO-OP screen in the languages of the game, from ONE table.

Writes
  launcher/menu/coop.str   the localized-strings file the engine reads (format of the stock localizedstrings/*.str: REFERENCE KEY + one
                           LANG_<NAME> line per language; CRLF; ends with ENDMARKER). The menu refers to a key as @COOP_<KEY>.
                           Bytes are in the code page of each language: Latin languages cp1252, Russian cp1251 (a game font indexes by byte).
  bridge/coop_text.h       the dynamic status texts for the bridge (it writes the status into a dvar), chosen by loc_language
                           (engine table 0x5A4BA0: 0 english, 1 french, 2 german, 3 italian, 4 spanish, 6 russian).

Languages: English (also the fallback), French, German, Italian, Spanish (the five of the Steam release) and Russian (a Russian
localization of the game exists). Translations are by the project author's assistant and have not been checked by native speakers.
Run:  python tools/make_coop_str.py
"""
import os

LANGS = ["ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH", "RUSSIAN"]
CODEPAGE = {"RUSSIAN": "cp1251"}   # everything else cp1252

MENU = {
    "HINT1": ["Host PC: press HOST.  Guest PC: press JOIN.",
              "PC h\u00f4te : HOST.  PC invit\u00e9 : JOIN.",
              "Host-PC: HOST.  Gast-PC: JOIN.",
              "PC host: HOST.  PC ospite: JOIN.",
              "PC anfitri\u00f3n: HOST.  PC invitado: JOIN.",
              "\u041f\u041a \u0445\u043e\u0441\u0442\u0430: HOST.  \u041f\u041a \u0433\u043e\u0441\u0442\u044f: JOIN."],
    "HINT2": ["Guest: leave auto, or type the host's address.",
              "Invit\u00e9 : laissez auto ou tapez l'adresse de l'h\u00f4te.",
              "Gast: auto lassen oder die Adresse des Hosts eingeben.",
              "Ospite: lascia auto o digita l'indirizzo dell'host.",
              "Invitado: deja auto o escribe la direcci\u00f3n del host.",
              "\u0413\u043e\u0441\u0442\u044c: \u043e\u0441\u0442\u0430\u0432\u044c\u0442\u0435 auto \u0438\u043b\u0438 \u0432\u0432\u0435\u0434\u0438\u0442\u0435 \u0430\u0434\u0440\u0435\u0441 \u0445\u043e\u0441\u0442\u0430."],
    "ADDR": ["Host address: ", "Adresse de l'h\u00f4te : ", "Host-Adresse: ", "Indirizzo host: ", "Direcci\u00f3n del host: ",
             "\u0410\u0434\u0440\u0435\u0441 \u0445\u043e\u0441\u0442\u0430: "],
    "STATUS": ["Status:", "Statut :", "Status:", "Stato:", "Estado:", "\u0421\u043e\u0441\u0442\u043e\u044f\u043d\u0438\u0435:"],
    "BACK": ["BACK", "RETOUR", "ZUR\u00dcCK", "INDIETRO", "VOLVER", "\u041d\u0410\u0417\u0410\u0414"],
    "WINDOW": ["Window:", "Fen\u00eatre :", "Fenster:", "Finestra:", "Ventana:", "\u041e\u043a\u043d\u043e:"],
    "BORDERLESS": ["BORDERLESS", "SANS BORDURE", "RANDLOS", "SENZA BORDI", "SIN BORDES", "\u0411\u0415\u0417 \u0420\u0410\u041c\u041a\u0418"],
    "NORMAL": ["NORMAL", "NORMAL", "NORMAL", "NORMALE", "NORMAL", "\u041e\u0411\u042b\u0427\u041d\u042b\u0419"],
    "MOUSE": ["Mouse:", "Souris :", "Maus:", "Mouse:", "Rat\u00f3n:", "\u041c\u044b\u0448\u044c:"],
    "RAW": ["RAW INPUT", "ENTR\u00c9E BRUTE", "RAW INPUT", "INPUT GREZZO", "ENTRADA DIRECTA", "\u041f\u0420\u042f\u041c\u041e\u0419 \u0412\u0412\u041e\u0414"],
    "HINT3": ["Host starts any mission, guest follows by itself. Log: coop5.log.",
              "L'h\u00f4te lance une mission, l'invit\u00e9 suit seul. Journal : coop5.log.",
              "Der Host startet eine Mission, der Gast folgt allein. Log: coop5.log.",
              "L'host avvia una missione, l'ospite segue da solo. Log: coop5.log.",
              "El anfitri\u00f3n inicia una misi\u00f3n y el invitado la sigue. Registro: coop5.log.",
              "\u0425\u043e\u0441\u0442 \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442 \u043b\u044e\u0431\u0443\u044e \u043c\u0438\u0441\u0441\u0438\u044e, \u0433\u043e\u0441\u0442\u044c \u0437\u0430\u0433\u0440\u0443\u0436\u0430\u0435\u0442 \u0435\u0451 \u0441\u0430\u043c. \u041b\u043e\u0433: coop5.log."],
    "HINT4": ["Both PCs: same Radmin / LAN network, firewall open for CoD2SP_s.exe.",
              "Les deux PC : m\u00eame r\u00e9seau Radmin / LAN, pare-feu ouvert pour CoD2SP_s.exe.",
              "Beide PCs: gleiches Radmin-/LAN-Netz, Firewall f\u00fcr CoD2SP_s.exe offen.",
              "Entrambi i PC: stessa rete Radmin / LAN, firewall aperto per CoD2SP_s.exe.",
              "Ambos PC: misma red Radmin / LAN, cortafuegos abierto para CoD2SP_s.exe.",
              "\u041e\u0431\u0430 \u041f\u041a: \u043e\u0434\u043d\u0430 \u0441\u0435\u0442\u044c Radmin / LAN, \u0431\u0440\u0430\u043d\u0434\u043c\u0430\u0443\u044d\u0440 \u043e\u0442\u043a\u0440\u044b\u0442 \u0434\u043b\u044f CoD2SP_s.exe."],
    "MISSION": ["Mission:",
          "Mission :",
          "Mission:",
          "Missione:",
          "Misi\u00f3n:",
          "\u041c\u0438\u0441\u0441\u0438\u044f:"],
    "FOLLOW_ON": ["FOLLOW HOST",
          "SUIVRE L'H\u00d4TE",
          "HOST FOLGEN",
          "SEGUI HOST",
          "SEGUIR AL ANFITRI\u00d3N",
          "\u0417\u0410 \u0425\u041e\u0421\u0422\u041e\u041c"],
    "FOLLOW_OFF": ["MANUAL",
          "MANUEL",
          "MANUELL",
          "MANUALE",
          "MANUAL",
          "\u0412\u0420\u0423\u0427\u041d\u0423\u042e"],
    "HINT5": ["Guest follows the host. F8: go to the partner.",
              "L'invit\u00e9 suit l'h\u00f4te. F8 : rejoindre le partenaire.",
              "Der Gast folgt dem Host. F8: zum Partner.",
              "L'ospite segue l'host. F8: raggiungi il compagno.",
              "El invitado sigue al anfitri\u00f3n. F8: ir al compa\u00f1ero.",
              "\u0413\u043e\u0441\u0442\u044c \u0441\u043b\u0435\u0434\u0443\u0435\u0442 \u0437\u0430 \u0445\u043e\u0441\u0442\u043e\u043c. F8: \u043a \u043d\u0430\u043f\u0430\u0440\u043d\u0438\u043a\u0443."],
    "PARTNER": ["Partner:",
          "Partenaire :",
          "Partner:",
          "Compagno:",
          "Compa\u00f1ero:",
          "\u041d\u0430\u043f\u0430\u0440\u043d\u0438\u043a:"],
    "WAITING": ["Waiting for the other player...",
          "En attente de l'autre joueur...",
          "Warte auf den anderen Spieler...",
          "In attesa dell'altro giocatore...",
          "Esperando al otro jugador...",
          "\u041e\u0436\u0438\u0434\u0430\u0435\u043c \u0432\u0442\u043e\u0440\u043e\u0433\u043e \u0438\u0433\u0440\u043e\u043a\u0430..."],
    "PARTNER_DOWN": ["Partner is down - back to the checkpoint",
          "Partenaire \u00e0 terre - retour au point de contr\u00f4le",
          "Partner gefallen - zur\u00fcck zum Kontrollpunkt",
          "Compagno caduto - ritorno al checkpoint",
          "Compa\u00f1ero ca\u00eddo - volviendo al punto de control",
          "\u041d\u0430\u043f\u0430\u0440\u043d\u0438\u043a \u043f\u043e\u0433\u0438\u0431 - \u0432\u043e\u0437\u0432\u0440\u0430\u0442 \u043a \u043a\u043e\u043d\u0442\u0440\u043e\u043b\u044c\u043d\u043e\u0439 \u0442\u043e\u0447\u043a\u0435"],
}

# dynamic status texts, at most 40 characters each (the dvar holds 40); %.20s is the host address
STATUS = [
    ("IDLE", ["Idle - pick HOST or JOIN", "Inactif - choisissez HOST ou JOIN", "Leerlauf - HOST oder JOIN w\u00e4hlen", "Inattivo - scegli HOST o JOIN",
              "Inactivo - elige HOST o JOIN", "\u041e\u0436\u0438\u0434\u0430\u043d\u0438\u0435: \u0432\u044b\u0431\u0435\u0440\u0438\u0442\u0435 HOST \u0438\u043b\u0438 JOIN"]),
    ("HOST_WAIT", ["HOST: waiting for the guest", "HOST : attente de l'invit\u00e9", "HOST: warte auf den Gast", "HOST: in attesa dell'ospite",
                   "HOST: esperando al invitado", "HOST: \u0436\u0434\u0451\u043c \u0433\u043e\u0441\u0442\u044f"]),
    ("HOST_CONN", ["HOST: guest connected", "HOST : invit\u00e9 connect\u00e9", "HOST: Gast verbunden", "HOST: ospite connesso",
                   "HOST: invitado conectado", "HOST: \u0433\u043e\u0441\u0442\u044c \u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0451\u043d"]),
    ("HOST_SILENT", ["HOST: guest is silent", "HOST : l'invit\u00e9 ne r\u00e9pond pas", "HOST: Gast antwortet nicht", "HOST: l'ospite non risponde",
                     "HOST: el invitado no responde", "HOST: \u0433\u043e\u0441\u0442\u044c \u043c\u043e\u043b\u0447\u0438\u0442"]),
    ("HOST_FOUND", ["HOST: guest found, start a mission", "HOST : invit\u00e9 trouv\u00e9, lancez une mission", "HOST: Gast gefunden, Mission starten",
                    "HOST: ospite trovato, avvia una missione", "HOST: invitado hallado, inicia misi\u00f3n",
                    "HOST: \u0433\u043e\u0441\u0442\u044c \u043d\u0430\u0439\u0434\u0435\u043d, \u043d\u0430\u0447\u043d\u0438\u0442\u0435 \u043c\u0438\u0441\u0441\u0438\u044e"]),
    ("JOIN_LOOK", ["JOIN: looking for the host", "JOIN : recherche de l'h\u00f4te", "JOIN: suche den Host", "JOIN: ricerca dell'host",
                   "JOIN: buscando al host", "JOIN: \u0438\u0449\u0435\u043c \u0445\u043e\u0441\u0442\u0430"]),
    ("JOIN_LINK", ["JOIN: linked to %.20s", "JOIN : connect\u00e9 \u00e0 %.20s", "JOIN: verbunden mit %.20s", "JOIN: collegato a %.20s",
                   "JOIN: conectado a %.20s", "JOIN: \u0441\u0432\u044f\u0437\u044c \u0441 %.20s"]),
    ("JOIN_START", ["JOIN: host found, wait for its mission", "JOIN : h\u00f4te trouv\u00e9, attendez sa mission", "JOIN: Host gefunden, Mission abwarten",
                    "JOIN: host trovato, attendi la missione", "JOIN: host hallado, espera su misi\u00f3n",
                    "JOIN: \u0445\u043e\u0441\u0442 \u043d\u0430\u0439\u0434\u0435\u043d, \u0436\u0434\u0438\u0442\u0435 \u043c\u0438\u0441\u0441\u0438\u044e"]),
    ("JOIN_NOANS", ["JOIN: no answer from the host", "JOIN : pas de r\u00e9ponse de l'h\u00f4te", "JOIN: keine Antwort vom Host",
                    "JOIN: nessuna risposta dall'host", "JOIN: sin respuesta del host",
                    "JOIN: \u0445\u043e\u0441\u0442 \u043d\u0435 \u043e\u0442\u0432\u0435\u0447\u0430\u0435\u0442"]),
]
STATUS.append(("WAITING", MENU["WAITING"]))      # shown on the loading screen while the game is held for the partner

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, "..")

def enc(lang, text):
    return text.encode(CODEPAGE.get(lang, "cp1252"))

# ---- coop.str builders. The engine takes the LAST LANG_ line of a record whatever the game language is (seen in the game: with all
# languages in one file the Russian line won in an English game), so a file carries ONE language. The iwd gets English; the bridge writes
# the file of the game's language to main\\localizedstrings\\coop.str (a loose file wins over the iwd) from the blobs in coop_text.h.
def build_str(langs):
    out = bytearray()
    def line(b): out.extend(b + b"\r\n")
    line(b'// CO-OP screen of cod2-coop. Generated by tools/make_coop_str.py - edit the table there, not this file.')
    line(b'// Translations are not checked by native speakers.')
    line(b'VERSION             "1"')
    line(b'CONFIG              "C:\\trees\\cod2\\bin\\StringEd.cfg"')
    line(b'FILENOTES           ""')
    line(b'')
    for key, texts in MENU.items():
        assert len(texts) == len(LANGS), key
        line(("REFERENCE           " + key).encode())
        for lang in langs:
            t = texts[LANGS.index(lang)]
            assert '"' not in t, (key, t)
            line(("LANG_%-14s" % lang).encode() + b'"' + enc(lang, t) + b'"')
        line(b'')
    line(b'')
    line(b'ENDMARKER')
    line(b'')
    line(b'')
    return bytes(out)

open(os.path.join(root, "launcher", "menu", "coop.str"), "wb").write(build_str(["ENGLISH"]))

# ---- coop_text.h (C string table; every non-ASCII byte is written as \xNN between separate literals so a hex escape never swallows a letter)
def cstr(b):
    parts, cur = [], ""
    for c in b:
        if c == 10:
            cur += chr(92) + 'n'
        elif c == 13:
            cur += chr(92) + 'r'
        elif 0x20 <= c < 0x80 and c != 0x22 and c != 0x5C:
            cur += chr(c)
        else:
            parts.append('"' + cur + '"'); cur = ""
            parts.append('"\\x%02X"' % c)
    parts.append('"' + cur + '"')
    return " ".join(parts)
h = ["/* Generated by tools/make_coop_str.py - do not edit. Status texts of the CO-OP screen by language: 0 en, 1 fr, 2 de, 3 it, 4 es, 5 ru.",
     "   Russian is in code page 1251 (the game font indexes by byte), the others in 1252. Each text is at most 40 characters. */",
     "enum { CT_IDLE, CT_HOST_WAIT, CT_HOST_CONN, CT_HOST_SILENT, CT_HOST_FOUND, CT_JOIN_LOOK, CT_JOIN_LINK, CT_JOIN_START, CT_JOIN_NOANS, CT_WAITING, CT_COUNT };",
     "static const char* const COOP_TXT[6][CT_COUNT] = {"]
for li, lang in enumerate(LANGS):
    row = []
    for key, texts in STATUS:
        b = enc(lang, texts[li])
        assert len(texts[li].replace("%.20s", "x" * 20)) <= 40, (lang, key, texts[li])
        row.append(cstr(b))
    h.append("    { " + ",\n      ".join(row) + " },")
h.append("};")
h.append("")
h.append("/* The whole coop.str of each language (what the bridge writes to main\\localizedstrings\\coop.str), same row order. */")
h.append("static const char* const COOP_STR[6] = {")
for lang in LANGS:
    h.append("    " + cstr(build_str([lang])) + ",")
h.append("};")

open(os.path.join(root, "bridge", "coop_text.h"), "w", newline="\n").write("\n".join(h) + "\n")
print("wrote launcher/menu/coop.str and bridge/coop_text.h")
