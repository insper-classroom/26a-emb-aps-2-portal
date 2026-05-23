#!/usr/bin/env python3

import sys
import glob
import serial
import pyautogui
pyautogui.PAUSE = 0  # Remove delay between actions
import tkinter as tk
from tkinter import ttk
from tkinter import messagebox
from time import sleep


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
    Loop principal que lê bytes da porta serial em loop infinito.
    Mantém o protocolo atual: 0xFF + axis (1 byte) + valor (2 bytes).
    """
    aguardando_sync = True
    payload = bytearray()

    while True:
        waiting = ser.in_waiting
        if waiting <= 0:
            sleep(0.001)
            continue

        chunk = ser.read(waiting)
        if not chunk:
            continue

        for b in chunk:
            if aguardando_sync:
                if b == 0xFF:
                    aguardando_sync = False
                    payload.clear()
                continue

            payload.append(b)
            if len(payload) == 3:
                axis, value = parse_data(payload)
                move_mouse(axis, value)
                aguardando_sync = True
                payload.clear()


def serial_ports():
    """Retorna uma lista das portas seriais disponíveis na máquina."""
    ports = []
    if sys.platform.startswith('win'):
        # Windows
        for i in range(1, 256):
            port = f'COM{i}'
            try:
                s = serial.Serial(port)
                s.close()
                ports.append(port)
            except (OSError, serial.SerialException):
                pass
    elif sys.platform.startswith('linux') or sys.platform.startswith('cygwin'):
        # Linux/Cygwin
        ports = glob.glob('/dev/tty[A-Za-z]*')
    elif sys.platform.startswith('darwin'):
        # macOS
        ports = glob.glob('/dev/tty.*')
    else:
        raise EnvironmentError('Plataforma não suportada para detecção de portas seriais.')

    result = []
    for port in ports:
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


def conectar_porta(port_name, root, botao_conectar, status_label, mudar_cor_circulo):
    """Abre a conexão com a porta selecionada e inicia o loop de leitura."""
    if not port_name:
        messagebox.showwarning("Aviso", "Selecione uma porta serial antes de conectar.")
        return

    ser = None
    try:
        ser = serial.Serial(port_name, 115200, timeout=0)
        ser.reset_input_buffer()
        status_label.config(text=f"Conectado em {port_name}", foreground="green")
        mudar_cor_circulo("green")
        botao_conectar.config(text="Conectado")  # Update button text to indicate connection
        root.update()

        # Inicia o loop de leitura (bloqueante).
        controle(ser)

    except KeyboardInterrupt:
        print("Encerrando via KeyboardInterrupt.")
    except Exception as e:
        messagebox.showerror("Erro de Conexão", f"Não foi possível conectar em {port_name}.\nErro: {e}")
        mudar_cor_circulo("red")
    finally:
        if ser and ser.is_open:
            ser.close()
        status_label.config(text="Conexão encerrada.", foreground="red")
        mudar_cor_circulo("red")


def criar_janela():
    root = tk.Tk()
    root.title("Controle de Mouse")
    root.geometry("400x250")
    root.resizable(False, False)

    # Dark mode color settings
    dark_bg = "#2e2e2e"
    dark_fg = "#ffffff"
    accent_color = "#007acc"
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

    # Updated combobox styling to match the dark GUI color
    style.configure("TCombobox",
                    fieldbackground=dark_bg,
                    background=dark_bg,
                    foreground=dark_fg,
                    padding=4)
    style.map("TCombobox", fieldbackground=[("readonly", dark_bg)])

    # Main content frame (upper portion)
    frame_principal = ttk.Frame(root, padding="20")
    frame_principal.pack(expand=True, fill="both")

    titulo_label = ttk.Label(frame_principal, text="Controle de Mouse", font=("Segoe UI", 14, "bold"))
    titulo_label.pack(pady=(0, 10))

    porta_var = tk.StringVar(value="")

    botao_conectar = ttk.Button(
        frame_principal,
        text="Conectar e Iniciar Leitura",
        style="Accent.TButton",
        command=lambda: conectar_porta(porta_var.get(), root, botao_conectar, status_label, mudar_cor_circulo)
    )
    botao_conectar.pack(pady=10)

    # Create footer frame with grid layout to host status label, port dropdown, and status circle
    footer_frame = tk.Frame(root, bg=dark_bg)
    footer_frame.pack(side="bottom", fill="x", padx=10, pady=(10, 0))

    # Left: Status label
    status_label = tk.Label(footer_frame, text="Aguardando seleção de porta...", font=("Segoe UI", 11),
                            bg=dark_bg, fg=dark_fg)
    status_label.grid(row=0, column=0, sticky="w")

    # Center: Port selection dropdown
    portas_disponiveis = serial_ports()
    if portas_disponiveis:
        porta_var.set(portas_disponiveis[0])
    port_dropdown = ttk.Combobox(footer_frame, textvariable=porta_var,
                                 values=portas_disponiveis, state="readonly", width=10)
    port_dropdown.grid(row=0, column=1, padx=10)

    # Right: Status circle (canvas)
    circle_canvas = tk.Canvas(footer_frame, width=20, height=20, highlightthickness=0, bg=dark_bg)
    circle_item = circle_canvas.create_oval(2, 2, 18, 18, fill="red", outline="")
    circle_canvas.grid(row=0, column=2, sticky="e")

    footer_frame.columnconfigure(1, weight=1)

    def mudar_cor_circulo(cor):
        circle_canvas.itemconfig(circle_item, fill=cor)

    root.mainloop()


if __name__ == "__main__":
    criar_janela()
