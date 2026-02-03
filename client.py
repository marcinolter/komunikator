import socket
import errno
import threading
import queue
import tkinter as tk
from tkinter import ttk, messagebox, simpledialog

MAX_LINE = 4096  # limit na długość linii


def conv_private(a: str, b: str) -> str:
    # Robimy stabilny identyfikator rozmowy prywatnej.
    # Sortujemy loginy, żeby "u:ala:ola" było zawsze takie samo, niezależnie kto zacznie.
    x, y = sorted([a, b])
    return f"u:{x}:{y}"


class ChatTab(ttk.Frame):
    # Jedna zakładka czatu (jedna konwersacja: prywatna albo grupa)
    def __init__(self, parent, conv_id: str, title: str, send_cb):
        super().__init__(parent)
        self.conv_id = conv_id     # np. "u:ala:ola" albo "g:1"
        self.title = title         # tytuł zakładki
        self.send_cb = send_cb     # callback do App, żeby wysłać wiadomość

        # okno rozmowy (Text tylko do odczytu)
        self.text = tk.Text(self, height=18, wrap="word", state="disabled")
        self.text.pack(fill="both", expand=True, padx=6, pady=6)

        # dolny pasek: pole wpisywania + przycisk Wyślij
        bottom = ttk.Frame(self)
        bottom.pack(fill="x", padx=6, pady=(0, 6))

        self.entry = ttk.Entry(bottom)
        self.entry.pack(side="left", fill="x", expand=True)
        self.entry.bind("<Return>", lambda e: self.on_send())  # Enter wysyła

        btn = ttk.Button(bottom, text="Wyślij", command=self.on_send)
        btn.pack(side="left", padx=(6, 0))

    def append(self, line: str):
        # Dopisujemy do okna rozmowy (Text jest disabled, więc chwilowo go odblokowujemy)
        self.text.configure(state="normal")
        self.text.insert("end", line + "\n")
        self.text.see("end")  # przewiń na dół
        self.text.configure(state="disabled")

    def on_send(self):
        # Kliknięcie "Wyślij" albo Enter:
        msg = self.entry.get().strip()
        if not msg:
            return
        self.entry.delete(0, "end")
        # Zamiast wysyłać tutaj, wołamy callback w App (tam jest socket)
        self.send_cb(self.conv_id, msg)


class App(tk.Tk):
    # Główna aplikacja (Tk root) – trzyma stan i robi networking
    def __init__(self):
        super().__init__()
        self.title("Komunikator")
        self.geometry("900x520")

        # --- networking ---
        self.sock = None
        self.recv_thread = None
        self.rx_queue = queue.Queue()  # wątek odbioru -> GUI

        # --- stan aplikacji ---
        self.username = None
        self.friends = []
        self.friend_status = {}  # login -> ONLINE/OFFLINE
        self.groups = {}         # gid -> (name, members)
        self.tabs = {}           # conv_id -> ChatTab

        self.status_var = tk.StringVar(value="Rozłączony")

        # najpierw ekran logowania
        self._build_login_ui()

        # pętla “polling” kolejki z wątku odbioru
        self.after(50, self._process_queue)

    # ------------- networking -------------
    def _connect(self, host: str, port: int):
        # łączy się z serwerem (tylko raz); startuje wątek odbioru
        if self.sock:
            return
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((host, port))
        self.sock = s

        self.recv_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self.recv_thread.start()

    def _send_line(self, line: str):
        # wysyła jedną linię tekstu do serwera
        if not self.sock:
            raise RuntimeError("Brak połączenia")

        # bezpieczeństwo: nie pozwalamy wysłać nowych linii w środku
        if "\n" in line or "\r" in line:
            line = line.replace("\n", " ").replace("\r", " ")

        data = (line + "\n").encode("utf-8", errors="replace")
        try:
            self.sock.sendall(data)
        except (BrokenPipeError, OSError) as e:
            # Broken pipe = serwer zamknął połączenie, a my jeszcze próbujemy wysłać
            if isinstance(e, BrokenPipeError) or getattr(e, "errno", None) in (
                errno.EPIPE, errno.ECONNRESET, errno.ENOTCONN
            ):
                self._disconnect("Połączenie przerwane")
                raise RuntimeError(
                    "Połączenie z serwerem zostało przerwane. Połącz się ponownie."
                )
            raise

    def _disconnect(self, reason: str = "Rozłączono"):
        # zamyka socket i czyści stan połączenia
        try:
            if self.sock:
                try:
                    self.sock.shutdown(socket.SHUT_RDWR)
                except Exception:
                    pass
                try:
                    self.sock.close()
                except Exception:
                    pass
        finally:
            self.sock = None
            self.status_var.set(reason)

    def _rx_loop(self):
        # Wątek odbioru: recv() -> składanie bufora -> linie po '\n' -> wrzucanie do queue
        buf = b""
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    # serwer zamknął połączenie
                    self.rx_queue.put(("sys", "Rozłączono z serwerem"))
                    break

                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    try:
                        text = line.decode("utf-8", errors="replace").strip("\r")
                    except Exception:
                        text = ""
                    if text:
                        self.rx_queue.put(("line", text))
        except Exception as e:
            self.rx_queue.put(("sys", f"Błąd odbioru: {e}"))
        finally:
            # pozwól GUI połączyć się ponownie
            self.sock = None

    # ------------- UI: login -------------
    def _build_login_ui(self):
        # ekran startowy: host/port + login/hasło + przyciski
        self.login_frame = ttk.Frame(self)
        self.login_frame.pack(fill="both", expand=True)

        box = ttk.Frame(self.login_frame)
        box.place(relx=0.5, rely=0.5, anchor="center")

        ttk.Label(box, text="Serwer").grid(row=0, column=0, sticky="w")
        self.host_var = tk.StringVar(value="127.0.0.1")
        self.port_var = tk.StringVar(value="5555")
        ttk.Entry(box, textvariable=self.host_var, width=18).grid(row=0, column=1, padx=6)
        ttk.Entry(box, textvariable=self.port_var, width=6).grid(row=0, column=2)

        ttk.Label(box, text="Login").grid(row=1, column=0, sticky="w", pady=(10, 0))
        self.user_var = tk.StringVar()
        ttk.Entry(box, textvariable=self.user_var, width=28).grid(
            row=1, column=1, columnspan=2, padx=6, pady=(10, 0)
        )

        ttk.Label(box, text="Hasło").grid(row=2, column=0, sticky="w")
        self.pass_var = tk.StringVar()
        ttk.Entry(box, textvariable=self.pass_var, show="*", width=28).grid(
            row=2, column=1, columnspan=2, padx=6
        )

        btns = ttk.Frame(box)
        btns.grid(row=3, column=0, columnspan=3, pady=12)
        ttk.Button(btns, text="Zaloguj", command=self._do_login).pack(side="left", padx=6)
        ttk.Button(btns, text="Zarejestruj", command=self._do_register).pack(side="left", padx=6)

        ttk.Label(self.login_frame, textvariable=self.status_var).pack(side="bottom", pady=8)

    def _build_main_ui(self):
        # główne UI po zalogowaniu: znajomi + grupy + zakładki czatów
        self.main_frame = ttk.Frame(self)
        self.main_frame.pack(fill="both", expand=True)

        # lewy panel: znajomi i grupy obok siebie
        left = ttk.Frame(self.main_frame, width=320)
        left.pack(side="left", fill="y", padx=6, pady=6)

        ttk.Label(left, text=f"Zalogowany: {self.username}").pack(anchor="w")

        body = ttk.Frame(left)
        body.pack(fill="both", expand=True, pady=(10, 0))

        body.grid_columnconfigure(0, weight=1)
        body.grid_columnconfigure(1, weight=1)
        body.grid_rowconfigure(0, weight=1)

        # --- Znajomi ---
        friends_box = ttk.LabelFrame(body, text="Znajomi")
        friends_box.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        friends_box.grid_rowconfigure(0, weight=1)
        friends_box.grid_columnconfigure(0, weight=1)

        self.friends_list = tk.Listbox(friends_box, height=16)
        self.friends_list.grid(row=0, column=0, sticky="nsew", padx=6, pady=6)

        ttk.Button(friends_box, text="Otwórz czat", command=self._open_friend_chat).grid(
            row=1, column=0, sticky="ew", padx=6, pady=(0, 6)
        )
        ttk.Button(friends_box, text="Dodaj", command=self._add_friend).grid(
            row=2, column=0, sticky="ew", padx=6, pady=(0, 6)
        )
        ttk.Button(friends_box, text="Odśwież", command=lambda: self._send_line("LIST_FRIENDS")).grid(
            row=3, column=0, sticky="ew", padx=6, pady=(0, 6)
        )

        # --- Grupy ---
        groups_box = ttk.LabelFrame(body, text="Grupy")
        groups_box.grid(row=0, column=1, sticky="nsew")
        groups_box.grid_rowconfigure(0, weight=1)
        groups_box.grid_columnconfigure(0, weight=1)

        self.groups_list = tk.Listbox(groups_box, height=16)
        self.groups_list.grid(row=0, column=0, sticky="nsew", padx=6, pady=6)

        ttk.Button(groups_box, text="Otwórz", command=self._open_group_chat).grid(
            row=1, column=0, sticky="ew", padx=6, pady=(0, 6)
        )
        ttk.Button(groups_box, text="Utwórz grupę", command=self._create_group).grid(
            row=2, column=0, sticky="ew", padx=6, pady=(0, 6)
        )
        ttk.Button(groups_box, text="Odśwież", command=lambda: self._send_line("LIST_GROUPS")).grid(
            row=3, column=0, sticky="ew", padx=6, pady=(0, 6)
        )

        ttk.Separator(left).pack(fill="x", pady=10)
        ttk.Button(left, text="Wyloguj", command=self._logout).pack(fill="x")

        # prawa strona: zakładki rozmów
        right = ttk.Frame(self.main_frame)
        right.pack(side="left", fill="both", expand=True, padx=6, pady=6)

        self.notebook = ttk.Notebook(right)
        self.notebook.pack(fill="both", expand=True)

        # statusbar na dole okna
        statusbar = ttk.Frame(self)
        statusbar.pack(side="bottom", fill="x")
        ttk.Label(statusbar, textvariable=self.status_var).pack(side="left", padx=8, pady=4)

    # --- helpers: friends presence ---
    def _extract_user(self, display: str) -> str:
        # z "login (online)" wyciągamy samo "login"
        return display.split(" ", 1)[0].strip()

    def _friend_display(self, user: str) -> str:
        # jak wyświetlać znajomego w listboxie (z online/offline)
        st = self.friend_status.get(user, "OFFLINE")
        pretty = "online" if st == "ONLINE" else "offline"
        return f"{user} ({pretty})"

    def _refresh_friends_listbox(self):
        # odświeżenie listy znajomych w GUI
        if not hasattr(self, "friends_list"):
            return
        self.friends_list.delete(0, "end")
        for u in self.friends:
            self.friends_list.insert("end", self._friend_display(u))

    # ------------- actions -------------
    def _do_register(self):
        # klik "Zarejestruj"
        host = self.host_var.get().strip()
        port = int(self.port_var.get().strip())
        user = self.user_var.get().strip()
        pw = self.pass_var.get().strip()
        try:
            self._connect(host, port)
            self._send_line(f"REGISTER {user} {pw}")
        except Exception as e:
            messagebox.showerror("Błąd", str(e))

    def _do_login(self):
        # klik "Zaloguj"
        host = self.host_var.get().strip()
        port = int(self.port_var.get().strip())
        user = self.user_var.get().strip()
        pw = self.pass_var.get().strip()
        try:
            self._connect(host, port)
            self._send_line(f"LOGIN {user} {pw}")
            self.username = user
        except Exception as e:
            messagebox.showerror("Błąd", str(e))

    def _logout(self):
        # wylogowanie: wysyłamy LOGOUT, rozłączamy, wracamy do login UI
        try:
            self._send_line("LOGOUT")
        except Exception:
            pass
        self._disconnect("Wylogowano")
        self._reset_to_login()

    def _reset_to_login(self):
        # czyści stan i buduje ekran logowania od nowa
        if hasattr(self, "main_frame"):
            self.main_frame.destroy()
        self.tabs.clear()
        self.groups.clear()
        self.friends.clear()
        self.friend_status.clear()
        self.username = None
        self.status_var.set("Wylogowano")
        if hasattr(self, "login_frame"):
            self.login_frame.destroy()
        self._build_login_ui()

    def _add_friend(self):
        # dodanie znajomego (dialog + komenda)
        friend = simpledialog.askstring("Dodaj znajomego", "Podaj login znajomego:")
        if not friend:
            return
        try:
            self._send_line(f"ADD_FRIEND {friend.strip()}")
        except Exception as e:
            messagebox.showerror("Błąd", str(e))

    def _create_group(self):
        # tworzenie grupy z checkboxami znajomych
        if not self.friends:
            messagebox.showinfo("Grupa", "Najpierw dodaj znajomych (min. 2), potem twórz grupę.")
            return

        win = tk.Toplevel(self)
        win.title("Utwórz grupę")
        win.geometry("360x420")
        win.transient(self)
        win.grab_set()

        ttk.Label(win, text="Nazwa grupy (bez spacji):").pack(anchor="w", padx=10, pady=(10, 0))
        name_var = tk.StringVar()
        name_entry = ttk.Entry(win, textvariable=name_var)
        name_entry.pack(fill="x", padx=10)
        name_entry.focus_set()

        ttk.Label(win, text="Wybierz znajomych do grupy (min. 2):").pack(anchor="w", padx=10, pady=(12, 0))

        box = ttk.Frame(win)
        box.pack(fill="both", expand=True, padx=10, pady=6)

        vars_ = {}
        for u in self.friends:
            v = tk.IntVar(value=0)
            vars_[u] = v
            ttk.Checkbutton(box, text=self._friend_display(u), variable=v).pack(anchor="w")

        btns = ttk.Frame(win)
        btns.pack(fill="x", padx=10, pady=10)

        def do_create():
            # walidacja + wysłanie CREATE_GROUP
            name = name_var.get().strip()
            if not name or " " in name or "\t" in name:
                messagebox.showwarning("Grupa", "Podaj nazwę grupy bez spacji.")
                return
            members = [u for u, v in vars_.items() if v.get() == 1]
            if len(members) < 2:
                messagebox.showwarning("Grupa", "Zaznacz co najmniej 2 znajomych (razem z Tobą będą 3 osoby).")
                return
            cmd = "CREATE_GROUP " + name + " " + " ".join(members)
            try:
                self._send_line(cmd)
                win.destroy()
            except Exception as e:
                messagebox.showerror("Błąd", str(e))

        ttk.Button(btns, text="Utwórz", command=do_create).pack(side="left", expand=True, fill="x")
        ttk.Button(btns, text="Anuluj", command=win.destroy).pack(side="left", expand=True, fill="x", padx=(8, 0))

    def _open_friend_chat(self):
        # otwieramy czat prywatny z zaznaczonym znajomym
        if not self.username:
            return
        sel = self.friends_list.curselection()
        if not sel:
            return
        friend = self._extract_user(self.friends_list.get(sel[0]))
        conv = conv_private(self.username, friend)
        self._open_tab(conv, f"{friend}")

    def _open_group_chat(self):
        # otwieramy czat grupowy z listy grup
        sel = self.groups_list.curselection()
        if not sel:
            return
        item = self.groups_list.get(sel[0])  # np. "g:12 | nazwa"
        gid = item.split("|", 1)[0].strip().replace("g:", "")
        conv = f"g:{gid}"
        title = item.split("|", 1)[1].strip() if "|" in item else conv
        self._open_tab(conv, title)

    def _open_tab(self, conv_id: str, title: str):
        # tworzy nową zakładkę czatu albo przełącza na istniejącą
        if conv_id in self.tabs:
            tab = self.tabs[conv_id]
            idx = self.notebook.index(tab)
            self.notebook.select(idx)
            return
        tab = ChatTab(self.notebook, conv_id, title, self._send_message)
        self.tabs[conv_id] = tab
        self.notebook.add(tab, text=title)
        self.notebook.select(tab)

    def _send_message(self, conv_id: str, msg: str):
        # wysyłanie wiadomości do serwera
        try:
            self._send_line(f"SEND_TO {conv_id} {msg}")
        except Exception as e:
            messagebox.showerror("Błąd", str(e))

    # ------------- parsing server lines -------------
    def _process_queue(self):
        # co 50ms: bierzemy z kolejki wszystko co przyszło z wątku recv
        try:
            while True:
                kind, payload = self.rx_queue.get_nowait()
                if kind == "sys":
                    self.status_var.set(payload)
                else:
                    self._handle_line(payload)
        except queue.Empty:
            pass
        self.after(50, self._process_queue)

    def _handle_line(self, line: str):
        # parser protokołu: OK / ERROR / EVENT ...
        if line.startswith("OK "):
            self.status_var.set(line)

            if line == "OK LOGIN":
                # sukces logowania -> budujemy główny UI i pobieramy listy
                self.login_frame.destroy()
                self._build_main_ui()
                self._send_line("LIST_FRIENDS")
                self._send_line("LIST_GROUPS")

            elif line.startswith("OK CREATE_GROUP"):
                # po stworzeniu grupy odśwież listę grup
                try:
                    self._send_line("LIST_GROUPS")
                except Exception:
                    pass

            elif line == "OK ADD_FRIEND":
                # po dodaniu znajomego odśwież listę znajomych
                try:
                    self._send_line("LIST_FRIENDS")
                except Exception:
                    pass
            return

        if line.startswith("ERROR "):
            # błąd z serwera – pokazujemy popup
            self.status_var.set(line)
            messagebox.showwarning("Serwer", line.replace("ERROR ", "", 1))
            return

        if line.startswith("EVENT FRIENDS"):
            # EVENT FRIENDS a,b,c
            friends_s = line[len("EVENT FRIENDS"):].strip()
            new_f = [x for x in friends_s.split(",") if x]

            # sprzątanie statusów, jeśli ktoś wypadł z listy
            for u in list(self.friend_status.keys()):
                if u not in new_f:
                    del self.friend_status[u]

            # domyślnie nieznani -> offline
            for u in new_f:
                self.friend_status.setdefault(u, "OFFLINE")

            self.friends = new_f
            self._refresh_friends_listbox()
            return

        if line.startswith("EVENT PRESENCE"):
            # EVENT PRESENCE <user> ONLINE|OFFLINE
            parts = line.split(" ", 3)
            if len(parts) >= 4:
                user = parts[2].strip()
                st = parts[3].strip().upper()
                if user:
                    self.friend_status[user] = "ONLINE" if st == "ONLINE" else "OFFLINE"
                    self._refresh_friends_listbox()
            return

        if line.startswith("EVENT GROUPINFO"):
            # EVENT GROUPINFO <id> <name> <m1,m2,m3>
            parts = line.split(" ", 3)
            if len(parts) < 4:
                return
            gid = parts[2]
            rest = parts[3]

            # rest: "name members"
            if " " in rest:
                name, members = rest.split(" ", 1)
            else:
                name, members = rest, ""

            self.groups[gid] = (name, [x for x in members.split(",") if x])

            # odśwież listbox grup
            if hasattr(self, "groups_list"):
                self.groups_list.delete(0, "end")
                for gid2, (name2, _) in sorted(self.groups.items(), key=lambda x: int(x[0])):
                    self.groups_list.insert("end", f"g:{gid2} | {name2}")
            return

        if line.startswith("EVENT MSG"):
            # EVENT MSG <conv_id> <from> <message...>
            parts = line.split(" ", 4)
            if len(parts) < 5:
                return
            conv_id = parts[2]
            from_user = parts[3]
            msg = parts[4]

            # tytuł zakładki: dla priv pokazujemy “druga strona”, dla grup nazwę grupy
            title = conv_id
            if conv_id.startswith("u:"):
                a, b = conv_id[2:].split(":", 1)
                title = b if a == self.username else a
            elif conv_id.startswith("g:"):
                gid = conv_id[2:]
                title = self.groups.get(gid, (conv_id, []))[0]

            # auto-otwieranie zakładki i dopisanie wiadomości
            if hasattr(self, "notebook"):
                self._open_tab(conv_id, title)
                self.tabs[conv_id].append(f"{from_user}: {msg}")
            return

        # jeśli przyszło coś nieznanego – wrzuć w statusbar
        self.status_var.set(line)


if __name__ == "__main__":
    # start GUI
    app = App()
    app.mainloop()
