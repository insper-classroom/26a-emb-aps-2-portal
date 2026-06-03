#!/usr/bin/env python3

import sys
import glob
import signal
import threading
import serial
import serial.tools.list_ports
import pyautogui
pyautogui.PAUSE = 0  # Remove delay entre ações
import tkinter as tk
from tkinter import ttk
from tkinter import messagebox


# --- Estado global da conexão ---
_ser = None
_parar = threading.Event()


def move_mouse(axis, value):
    """Move o mouse de acordo com o eixo e valor recebidos."""
    # Protocolo (ver main.c):
    # 0 - x (movimento horizontal)
    # 1 - y (movimento vertical)
    # 3 - r_click (botão direito)
    # 4 - l_click (botão esquerdo)
    # 5 - w, 6 - a, 7 - s, 8 - d, 9 - e (teclas)
    if axis == 0:
        pyautogui.moveRel(value, 0)
    elif axis == 1:
        pyautogui.moveRel(0, value)
    elif axis == 2:
        if value > 0:
            pyautogui.mouseDown(button='left')
        else:
            pyautogui.mouseUp(button='left')
    elif axis == 3:
        # right click event
        if value > 0:
            pyautogui.click(button='right')
    elif axis == 4:
        # left click event
        if value > 0:
            pyautogui.click(button='left')
    elif axis == 5:
        if value > 0:
            pyautogui.press('w')
    elif axis == 6:
        if value > 0:
            pyautogui.press('a')
    elif axis == 7:
        if value > 0:
            pyautogui.press('s')
    elif axis == 8:
        if value > 0:
            pyautogui.press('d')
    elif axis == 9:
        if value > 0:
            pyautogui.press('e')


def controle(ser):
    """
    Loop principal que lê bytes da porta serial.
    Aguarda o byte 0xFF e então lê 3 bytes: axis (1 byte) + valor (2 bytes).
    Encerra quando _parar for sinalizado.
    """
    while not _parar.is_set():
        sync_byte = ser.read(size=1)
        if not sync_byte:
            continue
        if sync_byte[0] == 0xFF:
            data = ser.read(size=3)
            if len(data) < 3:
                continue
            print(data)
            axis, value = parse_data(data)
            move_mouse(axis, value)


def serial_ports():
    """Retorna portas seriais disponíveis, incluindo portas rfcomm do Linux."""
    found = set()

    # Método 1: serial.tools (funciona bem para USB/COM, mas falha com rfcomm)
    for port in serial.tools.list_ports.comports():
        found.add(port.device)

    # Método 2: glob manual (captura /dev/rfcomm* e outros que o método 1 ignora)
    if sys.platform.startswith('linux') or sys.platform.startswith('cygwin'):
        for pattern in ['/dev/tty[A-Za-z]*', '/dev/rfcomm*']:
            for port in glob.glob(pattern):
                found.add(port)
    elif sys.platform.startswith('darwin'):
        for port in glob.glob('/dev/tty.*'):
            found.add(port)

    # Filtra apenas as portas que conseguem abrir
    result = []
    for port in sorted(found):
        try:
            s = serial.Serial(port)
            s.close()
            result.append(port)
        except (OSError, serial.SerialException):
            pass

    return result


def parse_data(data):
    """Interpreta os dados recebidos do buffer (axis + valor)."""
    axis = data[0]
    value = int.from_bytes(data[1:3], byteorder='little', signed=True)
    return axis, value


def conectar_porta(port_name, callbacks):
    """Abre a conexão com a porta selecionada e inicia o loop de leitura."""
    global _ser, _parar

    if not port_name:
        messagebox.showwarning("Aviso", "Selecione uma porta serial antes de conectar.")
        return

    _parar.clear()

    try:
        _ser = serial.Serial(port_name, 115200, timeout=1)
        callbacks['on_connect'](port_name)
        _ser.write("H".encode())  # Envia H após a conexão ser bem sucedida, podendo ser utilizado como Handshake.
        controle(_ser)

    except KeyboardInterrupt:
        print("Encerrando via KeyboardInterrupt.")
    except Exception as e:
        if not _parar.is_set():  # Só exibe erro se não foi desconexão intencional
            messagebox.showerror("Erro de Conexão", f"Não foi possível conectar em {port_name}.\nErro: {e}")
    finally:
        if _ser and _ser.is_open:
            _ser.close()
        callbacks['on_disconnect']()


def desconectar():
    """Sinaliza o loop para parar e fecha a porta serial."""
    global _ser, _parar
    _parar.set()
    if _ser and _ser.is_open:
        _ser.close()


def criar_janela():
    root = tk.Tk()
    root.title("Controle de Mouse")
    root.geometry("400x300")
    root.resizable(False, False)

    dark_bg = "#2e2e2e"
    dark_fg = "#ffffff"
    accent_color = "#007acc"
    danger_color = "#c0392b"
    root.configure(bg=dark_bg)

    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure("TFrame", background=dark_bg)
    style.configure("TLabel", background=dark_bg, foreground=dark_fg, font=("Segoe UI", 11))
    style.configure("TButton", font=("Segoe UI", 10, "bold"),
                    foreground=dark_fg, background="#444444", borderwidth=0)
    style.map("TButton", background=[("active", "#555555")])
    style.configure("Accent.TButton", font=("Segoe UI", 12, "bold"),
                    foreground=dark_fg, background=accent_color, padding=6)
    style.map("Accent.TButton", background=[("active", "#005f9e")])
    style.configure("Danger.TButton", font=("Segoe UI", 12, "bold"),
                    foreground=dark_fg, background=danger_color, padding=6)
    style.map("Danger.TButton", background=[("active", "#922b21")])
    style.configure("TCombobox",
                    fieldbackground=dark_bg,
                    background=dark_bg,
                    foreground=dark_fg,
                    padding=4)
    style.map("TCombobox", fieldbackground=[("readonly", dark_bg)])

    frame_principal = ttk.Frame(root, padding="20")
    frame_principal.pack(expand=True, fill="both")

    titulo_label = ttk.Label(frame_principal, text="Controle de Mouse", font=("Segoe UI", 14, "bold"))
    titulo_label.pack(pady=(0, 10))

    porta_var = tk.StringVar(value="")

    # --- Frame dos botões lado a lado ---
    frame_botoes = ttk.Frame(frame_principal)
    frame_botoes.pack(pady=10)

    def on_connect(port_name):
        root.after(0, lambda: [
            status_label.config(text=f"Conectado em {port_name}", foreground="green"),
            mudar_cor_circulo("green"),
            botao_conectar.config(state="disabled"),
            botao_desconectar.config(state="normal"),
            port_dropdown.config(state="disabled"),
        ])

    def on_disconnect():
        root.after(0, lambda: [
            status_label.config(text="Conexão encerrada.", foreground="red"),
            mudar_cor_circulo("red"),
            botao_conectar.config(state="normal"),
            botao_desconectar.config(state="disabled"),
            port_dropdown.config(state="readonly"),
        ])

    callbacks = {'on_connect': on_connect, 'on_disconnect': on_disconnect}

    def iniciar():
        t = threading.Thread(
            target=conectar_porta,
            args=(porta_var.get(), callbacks),
            daemon=True
        )
        t.start()

    def encerrar():
        desconectar()

    botao_conectar = ttk.Button(
        frame_botoes,
        text="Conectar",
        style="Accent.TButton",
        command=iniciar
    )
    botao_conectar.pack(side="left", padx=(0, 8))

    botao_desconectar = ttk.Button(
        frame_botoes,
        text="Desconectar",
        style="Danger.TButton",
        state="disabled",
        command=encerrar
    )
    botao_desconectar.pack(side="left")

    # --- Checkbox FAILSAFE ---
    failsafe_var = tk.BooleanVar(value=True)  # Ativo por padrão

    def toggle_failsafe():
        pyautogui.FAILSAFE = failsafe_var.get()

    tk.Checkbutton(
        frame_principal,
        text="Failsafe (mover mouse ao canto encerra)",
        variable=failsafe_var,
        command=toggle_failsafe,
        bg=dark_bg, fg="#ffb74d",
        selectcolor=dark_bg,
        activebackground=dark_bg,
        activeforeground="#ffb74d",
        font=("Segoe UI", 9),
    ).pack(pady=(0, 4))

    # --- Footer ---
    footer_frame = tk.Frame(root, bg=dark_bg)
    footer_frame.pack(side="bottom", fill="x", padx=10, pady=(10, 0))

    status_label = tk.Label(footer_frame, text="Aguardando seleção de porta...", font=("Segoe UI", 11),
                            bg=dark_bg, fg=dark_fg)
    status_label.grid(row=0, column=0, sticky="w")

    portas_disponiveis = serial_ports()
    if portas_disponiveis:
        porta_var.set(portas_disponiveis[0])
    port_dropdown = ttk.Combobox(footer_frame, textvariable=porta_var,
                                 values=portas_disponiveis, state="readonly", width=10)
    port_dropdown.grid(row=0, column=1, padx=10)

    circle_canvas = tk.Canvas(footer_frame, width=20, height=20, highlightthickness=0, bg=dark_bg)
    circle_item = circle_canvas.create_oval(2, 2, 18, 18, fill="red", outline="")
    circle_canvas.grid(row=0, column=2, sticky="e")

    footer_frame.columnconfigure(1, weight=1)

    def mudar_cor_circulo(cor):
        circle_canvas.itemconfig(circle_item, fill=cor)

    # Ctrl+C no terminal e no foco da janela
    def handle_sigint(sig, frame):
        print("\nCtrl+C recebido — encerrando.")
        desconectar()
        root.after(0, root.destroy)

    signal.signal(signal.SIGINT, handle_sigint)

    # Mantém o loop tkinter responsivo ao SIGINT no Windows/Linux
    def checar_sigint():
        root.after(200, checar_sigint)

    root.after(200, checar_sigint)

    # Ctrl+C com foco na janela
    root.bind("<Control-c>", lambda e: handle_sigint(None, None))

    # Fechar a janela também desconecta
    root.protocol("WM_DELETE_WINDOW", lambda: [desconectar(), root.destroy()])

    root.mainloop()


if __name__ == "__main__":
    criar_janela()
