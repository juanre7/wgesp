"""Tests de check_deps.py. Corren en el PC y sin red: la unica funcion que
sale a internet (ultima) se prueba con un urlopen de mentira.

    python3 -m pytest scripts/
"""

import io
import json
import sys
import pathlib

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import check_deps as cd


# ---- dependencias: el parser de idf_component.yml sin PyYAML ---------------

def manifiesto(tmp_path, texto):
    p = tmp_path / "idf_component.yml"
    p.write_text(texto)
    return p


def test_dependencias_lee_el_bloque(tmp_path):
    m = manifiesto(tmp_path, """
dependencies:
  espressif/mdns: "^1.8.2"
  espressif/libsodium: '~1.0'
""")
    assert list(cd.dependencias(m)) == [
        ("espressif/mdns", "^1.8.2"),
        ("espressif/libsodium", "~1.0"),
    ]


def test_dependencias_ignora_lo_que_no_es_del_registro(tmp_path):
    """Sin namespace (idf), sin rango, comentarios, y todo lo que este fuera
    del bloque dependencies."""
    m = manifiesto(tmp_path, """
version: "1.0.0"
description: algo
dependencies:
  # un comentario
  idf: ">=5.5"
  espressif/mdns: "^1.8.2"
  espressif/sin_rango:
targets:
  esp32c6/otro: "^1.0"
""")
    assert list(cd.dependencias(m)) == [("espressif/mdns", "^1.8.2")]


def test_dependencias_sin_bloque(tmp_path):
    assert list(cd.dependencias(manifiesto(tmp_path, 'version: "1.0"\n'))) == []


# ---- cubre: la semantica de rangos ----------------------------------------

@pytest.mark.parametrize("rango,version,esperado", [
    ("^1.0.20", (1, 0, 20), True),    # el minimo entra
    ("^1.0.20", (1, 9, 3), True),     # cualquier menor por encima
    ("^1.0.20", (1, 0, 19), False),   # por debajo del minimo
    ("^1.0.20", (2, 0, 0), False),    # mayor nueva: se ha quedado atras
    ("^0.4.1", (0, 4, 9), True),      # con mayor 0 manda la menor
    ("^0.4.1", (0, 5, 0), False),
    ("~1.4", (1, 4, 0), True),
    ("~1.4", (1, 4, 99), True),
    ("~1.4", (1, 5, 0), False),
    ("1.0.20", (1, 0, 20), True),     # sin prefijo: exacta
    ("1.0.20", (1, 0, 21), False),
    ("  ^1.8.2  ", (1, 8, 3), True),  # los espacios no cuentan
    ("^2", (2, 7, 0), True),          # solo mayor
])
def test_cubre(rango, version, esperado):
    assert cd.cubre(rango, version) is esperado


@pytest.mark.parametrize("rango", [">=1.4", "*", "1.x", "", "^1.2.3.4"])
def test_cubre_rango_no_reconocido(rango):
    """None, no False: no es que este atrasada, es que no sabemos decirlo."""
    assert cd.cubre(rango, (1, 0, 0)) is None


# ---- badge -----------------------------------------------------------------

def test_badge_al_dia():
    b = json.loads(cd.badge(2, 0))
    assert b["message"] == "2 up to date"
    assert b["color"] == "brightgreen"
    assert b["schemaVersion"] == 1 and b["label"] == "deps"


def test_badge_atrasadas():
    b = json.loads(cd.badge(3, 1))
    assert b["message"] == "1 of 3 outdated"
    assert b["color"] == "orange"


def test_badge_acaba_en_salto_de_linea():
    """Se commitea: sin el salto, git marca 'no newline at end of file'."""
    assert cd.badge(1, 0).endswith("}\n")


# ---- ultima: lo unico que toca la red -------------------------------------

def urlopen_falso(payload):
    class Respuesta(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

    return lambda url, timeout=None: Respuesta(json.dumps(payload).encode())


def test_ultima_coge_la_mayor_final(monkeypatch):
    monkeypatch.setattr(cd.urllib.request, "urlopen", urlopen_falso(
        {"versions": [{"version": "1.0.9"}, {"version": "1.2.0"}, {"version": "1.1.5"}]}))
    assert cd.ultima("espressif/mdns") == (1, 2, 0)


def test_ultima_ignora_prereleases(monkeypatch):
    monkeypatch.setattr(cd.urllib.request, "urlopen", urlopen_falso(
        {"versions": [{"version": "1.0.9"}, {"version": "2.0.0-beta1"}, {"version": "1.1.0~1"}]}))
    assert cd.ultima("espressif/mdns") == (1, 0, 9)


def test_ultima_sin_finales(monkeypatch):
    monkeypatch.setattr(cd.urllib.request, "urlopen", urlopen_falso(
        {"versions": [{"version": "0.1.0-rc1"}]}))
    with pytest.raises(SystemExit):
        cd.ultima("espressif/loquesea")


# ---- main: el codigo de salida y el badge ---------------------------------

def repo(tmp_path, rango):
    (tmp_path / "main").mkdir()
    (tmp_path / "main" / "idf_component.yml").write_text(
        f'dependencies:\n  espressif/mdns: "{rango}"\n')
    return tmp_path


def test_main_al_dia(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(cd, "ultima", lambda n: (1, 8, 3))
    destino = tmp_path / "deps.json"
    assert cd.main(str(repo(tmp_path, "^1.8.2")), str(destino)) == 0
    assert json.loads(destino.read_text())["message"] == "1 up to date"
    assert "OK espressif/mdns" in capsys.readouterr().out


def test_main_atrasada(tmp_path, monkeypatch, capsys):
    """Sale 1 y el badge se escribe igual: publicar el recuento es su trabajo."""
    monkeypatch.setattr(cd, "ultima", lambda n: (2, 0, 0))
    destino = tmp_path / "deps.json"
    assert cd.main(str(repo(tmp_path, "^1.8.2")), str(destino)) == 1
    assert json.loads(destino.read_text())["message"] == "1 of 1 outdated"
    assert "::warning" in capsys.readouterr().out


def test_main_rango_raro_cuenta_como_atrasada(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(cd, "ultima", lambda n: (1, 8, 3))
    assert cd.main(str(repo(tmp_path, ">=1.8"))) == 1
    assert "rango no reconocido" in capsys.readouterr().out


def test_main_sin_badge_no_escribe_nada(tmp_path, monkeypatch):
    monkeypatch.setattr(cd, "ultima", lambda n: (1, 8, 3))
    r = repo(tmp_path, "^1.8.2")
    assert cd.main(str(r)) == 0
    assert not (r / "deps.json").exists()


def test_main_sin_dependencias(tmp_path, monkeypatch):
    destino = tmp_path / "deps.json"
    assert cd.main(str(tmp_path), str(destino)) == 0
    assert json.loads(destino.read_text())["message"] == "0 up to date"


# ---- cli: los dos argumentos ----------------------------------------------

def test_cli_raiz_y_badge(tmp_path, monkeypatch):
    monkeypatch.setattr(cd, "ultima", lambda n: (1, 8, 3))
    r = repo(tmp_path, "^1.8.2")
    destino = tmp_path / "b.json"
    assert cd.cli([str(r), "--badge", str(destino)]) == 0
    assert destino.exists()


def test_cli_badge_delante_de_la_raiz(tmp_path, monkeypatch):
    """--badge se saca de la lista, venga donde venga."""
    monkeypatch.setattr(cd, "ultima", lambda n: (2, 0, 0))
    r = repo(tmp_path, "^1.8.2")
    destino = tmp_path / "b.json"
    assert cd.cli(["--badge", str(destino), str(r)]) == 1
    assert destino.exists()


def test_cli_sin_argumentos_usa_el_directorio_actual(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    assert cd.cli([]) == 0
