"""
Análise de dados de campo magnético
Lê data.txt, idx.txt e current.txt e gera gráfico com:
  - Eixo X: corrente do eletroímã (current.txt)
  - Curva 1: campo magnético (valores de idx.txt)
  - Curva 2: Vraw médio por ponto de campo (data.txt + idx.txt)
"""

import re
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np
from pathlib import Path


# ── Leitura de idx.txt ────────────────────────────────────────────────────────
def parse_idx(path: str) -> dict[int, tuple[int, int]]:
    """Retorna {campo: (idx_inicio, idx_fim)}"""
    resultado = {}
    pattern = re.compile(r"^\s*(-?\d+)\s*:\s*(\d+)\s*[-–]\s*(\d+)")
    with open(path, encoding="utf-8") as f:
        for linha in f:
            m = pattern.match(linha)
            if m:
                campo = int(m.group(1))
                inicio = int(m.group(2))
                fim = int(m.group(3))
                resultado[campo] = (inicio, fim)
    return resultado


# ── Leitura de current.txt ────────────────────────────────────────────────────
def parse_current(path: str) -> dict[int, float]:
    """Retorna {campo: corrente}"""
    resultado = {}
    pattern = re.compile(r"^\s*(-?\d+)\s*:\s*(-?[\d.]+)")
    with open(path, encoding="utf-8") as f:
        for linha in f:
            m = pattern.match(linha)
            if m:
                campo = int(m.group(1))
                corrente = float(m.group(2))
                resultado[campo] = corrente
    return resultado


# ── Leitura de data.txt ───────────────────────────────────────────────────────
def parse_data(path: str) -> dict[int, float]:
    """Retorna {idx: Vraw_em_mV}"""
    resultado = {}
    pattern = re.compile(r"Vraw\s*=\s*(-?[\d.]+)\s*mV.*Idx:\s*(\d+)", re.IGNORECASE)
    with open(path, encoding="utf-8") as f:
        for linha in f:
            m = pattern.search(linha)
            if m:
                vraw = float(m.group(1))
                idx = int(m.group(2))
                resultado[idx] = vraw
    return resultado


# ── Cálculo de Vraw médio por campo ──────────────────────────────────────────
def calcular_vraw_por_campo(
    data: dict[int, float],
    idx_map: dict[int, tuple[int, int]],
) -> dict[int, float]:
    """Para cada campo, calcula a média dos Vraw dentro do intervalo de índices."""
    resultado = {}
    for campo, (inicio, fim) in idx_map.items():
        valores = [data[i] for i in range(inicio, fim + 1) if i in data]
        if valores:
            resultado[campo] = float(np.mean(valores))
        else:
            print(f"  Aviso: nenhum dado encontrado para campo={campo} "
                  f"(idx {inicio}–{fim})")
    return resultado


# ── Plot ─────────────────────────────────────────────────────────────────────
def plot(correntes, campos, vraws):
    fig, ax1 = plt.subplots(figsize=(10, 6))
    fig.patch.set_facecolor("#0f1117")
    ax1.set_facecolor("#181c27")

    cor_campo = "#4fc3f7"   # azul claro – campo magnético
    cor_vraw  = "#ff7043"   # laranja    – Vraw

    # --- Curva 1: campo magnético (eixo Y esquerdo) --------------------------
    lns1 = ax1.plot(
        correntes, campos,
        color=cor_campo, linewidth=2.2, marker="o", markersize=5,
        label="Campo magnético (unidade original)",
    )
    ax1.set_xlabel("Corrente do eletroímã (A)", color="white", fontsize=12)
    ax1.set_ylabel("Campo magnético", color=cor_campo, fontsize=12)
    ax1.tick_params(axis="y", colors=cor_campo)
    ax1.tick_params(axis="x", colors="white")
    for spine in ax1.spines.values():
        spine.set_edgecolor("#3a3f55")

    # --- Curva 2: Vraw (eixo Y direito) --------------------------------------
    ax2 = ax1.twinx()
    ax2.set_facecolor("none")
    lns2 = ax2.plot(
        correntes, vraws,
        color=cor_vraw, linewidth=2.2, marker="s", markersize=5,
        linestyle="--", label="Vraw médio (mV)",
    )
    ax2.set_ylabel("Vraw (mV)", color=cor_vraw, fontsize=12)
    ax2.tick_params(axis="y", colors=cor_vraw)
    for spine in ax2.spines.values():
        spine.set_edgecolor("#3a3f55")

    # --- Grade e legenda -----------------------------------------------------
    ax1.grid(True, color="#2a2f45", linestyle="--", linewidth=0.7, alpha=0.8)
    ax1.xaxis.set_minor_locator(ticker.AutoMinorLocator())
    ax1.yaxis.set_minor_locator(ticker.AutoMinorLocator())

    todas_linhas = lns1 + lns2
    labels = [l.get_label() for l in todas_linhas]
    ax1.legend(todas_linhas, labels,
               facecolor="#1e2235", edgecolor="#3a3f55",
               labelcolor="white", fontsize=10, loc="upper left")

    plt.title(
        "Campo magnético e Vraw em função da corrente",
        color="white", fontsize=14, fontweight="bold", pad=14,
    )
    fig.tight_layout()

    out = Path("grafico_campo.png")
    plt.savefig(out, dpi=150, bbox_inches="tight", facecolor=fig.get_facecolor())
    print(f"\nGráfico salvo em: {out.resolve()}")
    plt.show()


# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    data_path    = "data.txt"
    idx_path     = "idx.txt"
    current_path = "current.txt"

    print("Lendo arquivos…")
    idx_map  = parse_idx(idx_path)
    currents = parse_current(current_path)
    data     = parse_data(data_path)

    print(f"  idx.txt    → {len(idx_map)} pontos de campo")
    print(f"  current.txt → {len(currents)} pontos de corrente")
    print(f"  data.txt   → {len(data)} leituras de Vraw")

    vraw_por_campo = calcular_vraw_por_campo(data, idx_map)

    # Campos presentes nos três arquivos
    campos_validos = sorted(
        set(idx_map) & set(currents) & set(vraw_por_campo)
    )
    if not campos_validos:
        raise RuntimeError(
            "Nenhum campo em comum entre idx.txt, current.txt e data.txt. "
            "Verifique os arquivos."
        )

    correntes = [currents[c]       for c in campos_validos]
    campos    = [c                 for c in campos_validos]
    vraws     = [vraw_por_campo[c] for c in campos_validos]

    print(f"\n{len(campos_validos)} pontos em comum para o gráfico.")
    print(f"  Corrente : {min(correntes):.2f} A … {max(correntes):.2f} A")
    print(f"  Campo    : {min(campos)} … {max(campos)}")
    print(f"  Vraw     : {min(vraws):.4f} mV … {max(vraws):.4f} mV")

    plot(correntes, campos, vraws)


if __name__ == "__main__":
    main()