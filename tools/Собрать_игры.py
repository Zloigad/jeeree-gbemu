#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Собери свои игры — детская оболочка над make_roms_uf2.py.

Двойной клик -> большое окно -> кнопка «Выбрать игры» -> кнопка «Собрать».
Нужен только Python с python.org (там уже есть tkinter), ничего ставить
через pip не надо.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

try:
    import make_roms_uf2 as pack
except ImportError as ex:
    sys.exit("Рядом нет make_roms_uf2.py (%s). Положи его в ту же папку." % ex)

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError:
    # Консоли может не быть видно (.bat запускает без окна), поэтому показываем
    # ошибку окном Windows, а не print.
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(
            0,
            "Python встал без tkinter (окна нечем рисовать).\n\n"
            "Поставь Python с python.org со всеми галочками по умолчанию\n"
            "и запусти снова.",
            "Нужен Python с tkinter", 0x10)
    except Exception:
        pass
    sys.exit(1)

GAMES_DIR = os.path.join(HERE, "Мои_игры_класть_сюда")
OUT_FILE = os.path.join(HERE, "games.uf2")
CAP_KIB = pack.CAPACITY // 1024          # 1516 (v9.0)


def kid_reason(why):
    """Причина пропуска человеческим языком (коротко, для списка)."""
    if why.startswith("образ больше всей области"):
        return ("слишком большая: игры больше %d КБ не влезают никогда"
                " (убери её из списка)" % pack.single_max_kib())
    if why.startswith("не влезло по месту"):
        return "не хватает места: убери что-нибудь из списка"
    if why.startswith("список заполнен"):
        return "игр больше 48 — столько список не держит"
    return why


def kib(n_bytes):
    return (n_bytes + 1023) // 1024


def reject_reason(inf):
    """Почему файл нельзя брать в детскую сборку (None — можно)."""
    if getattr(inf, "cgb_only", False):
        return ("игра ТОЛЬКО для Game Boy Color — "
                "на консоли НЕ пойдёт (ядро — классический Game Boy)")
    return None


class App:
    def __init__(self, root):
        self.root = root
        self.infos = []          # Info в порядке списка
        root.title("Консоль — собери свои игры")
        root.geometry("700x740")
        root.minsize(620, 640)

        title = tk.Label(root, text="🎮 СОБЕРИ СВОИ ИГРЫ",
                         font=("Segoe UI", 22, "bold"))
        title.pack(pady=(12, 2))
        tk.Label(root, text="3 шага: выбери игры → собери → перетащи на консоль",
                 font=("Segoe UI", 12)).pack(pady=(0, 8))

        # --- шаг 1 ---
        f1 = tk.LabelFrame(root, text=" ① Выбери игры (классический Game Boy) ",
                           font=("Segoe UI", 13, "bold"), padx=10, pady=8)
        f1.pack(fill="both", expand=True, padx=12, pady=4)
        btn_row = tk.Frame(f1)
        btn_row.pack(fill="x")
        tk.Button(btn_row, text="📂 ВЫБРАТЬ ИГРЫ…", font=("Segoe UI", 13, "bold"),
                  bg="#2e7d32", fg="white", activebackground="#388e3c",
                  activeforeground="white", padx=12, pady=6,
                  command=self.pick_files).pack(side="left", padx=(0, 6))
        tk.Button(btn_row, text="📁 Из папки «Мои игры»",
                  font=("Segoe UI", 11), padx=8,
                  command=self.pick_mygames).pack(side="left", padx=6)
        tk.Button(btn_row, text="❌ Убрать", font=("Segoe UI", 11),
                  command=self.remove_sel).pack(side="right", padx=(6, 0))
        tk.Button(btn_row, text="🧹 Очистить", font=("Segoe UI", 11),
                  command=self.clear_all).pack(side="right")

        list_frame = tk.Frame(f1)
        list_frame.pack(fill="both", expand=True, pady=(8, 4))
        scroll = tk.Scrollbar(list_frame)
        scroll.pack(side="right", fill="y")
        self.listbox = tk.Listbox(list_frame, font=("Consolas", 12),
                                  yscrollcommand=scroll.set, height=9)
        self.listbox.pack(side="left", fill="both", expand=True)
        scroll.config(command=self.listbox.yview)

        cap_row = tk.Frame(f1)
        cap_row.pack(fill="x", pady=(2, 0))
        self.cap_label = tk.Label(cap_row, text="", font=("Segoe UI", 12, "bold"))
        self.cap_label.pack(side="left")
        self.bar = ttk.Progressbar(cap_row, length=220, mode="determinate",
                                   maximum=100)
        self.bar.pack(side="right")

        # --- шаг 2 ---
        f2 = tk.LabelFrame(root, text=" ② Собери файл для консоли ",
                           font=("Segoe UI", 13, "bold"), padx=10, pady=8)
        f2.pack(fill="x", padx=12, pady=4)
        self.build_btn = tk.Button(f2, text="🔨 СОБРАТЬ games.uf2",
                                   font=("Segoe UI", 15, "bold"),
                                   bg="#1565c0", fg="white",
                                   activebackground="#1976d2",
                                   activeforeground="white", padx=12, pady=8,
                                   command=self.build)
        self.build_btn.pack(fill="x")
        self.result = tk.Label(f2, text="Пока ничего не собрано.",
                               font=("Segoe UI", 12), wraplength=620,
                               justify="left")
        self.result.pack(fill="x", pady=(6, 0))

        # --- шаг 3 ---
        f3 = tk.LabelFrame(root, text=" ③ Залей в консоль ",
                           font=("Segoe UI", 13, "bold"), padx=10, pady=8)
        f3.pack(fill="x", padx=12, pady=4)
        tk.Label(f3, font=("Segoe UI", 12), wraplength=620, justify="left",
                 text="1) Подключи консоль к ПК.\n"
                      "2) ЗАЖМИ кнопку MENU и, не отпуская, коротко нажми START —\n"
                      "    появится диск RPI-RP2.\n"
                      "3) Перетащи на него файл games.uf2.\n"
                      "Диск исчезнет сам — значит, всё получилось ✅").pack(anchor="w")
        tk.Button(f3, text="📂 Открыть папку с готовым games.uf2",
                  font=("Segoe UI", 11), command=self.open_out_dir).pack(pady=(6, 0))

        self.refit()

    # ---------- список ----------
    def log(self, text):
        self.result.config(text=text)

    def add_paths(self, paths):
        added, bad, refused = 0, [], []
        known = {os.path.abspath(i.path) for i in self.infos}
        for p in sorted(paths):
            if not os.path.isfile(p):
                continue
            if os.path.abspath(p) in known:
                continue
            try:
                inf = pack.load_rom(p)
            except Exception as ex:
                bad.append("%s — %s" % (os.path.basename(p), ex))
                continue
            rej = reject_reason(inf)
            if rej is not None:
                refused.append("%s — %s" % (os.path.basename(p), rej))
                continue
            self.infos.append(inf)
            known.add(os.path.abspath(p))
            added += 1
        self.refit()
        if bad:
            messagebox.showwarning(
                "Не похоже на игры",
                "Эти файлы пропущены:\n\n" + "\n".join(bad) +
                "\n\nНужны файлы игр Game Boy (.gb).")
        if refused:
            messagebox.showwarning(
                "Такие игры консоль не тянет",
                "Эти игры — ТОЛЬКО для Game Boy Color, на консоли они НЕ пойдут:\n\n" +
                "\n".join(refused) +
                "\n\nНужны обычные игры классического Game Boy (чёрно-белые).")
        if added:
            warns = ["%s: %s" % (i.name, "; ".join(i.warns))
                     for i in self.infos[-added:] if i.warns]
            if warns:
                self.log("⚠ Замечания:\n" + "\n".join(warns))

    def pick_files(self):
        paths = filedialog.askopenfilenames(
            title="Выбери игры",
            initialdir=GAMES_DIR if os.path.isdir(GAMES_DIR) else HERE,
            filetypes=[("Игры Game Boy (кроме Color-only)", "*.gb *.gbc *.sgb"),
                       ("Все файлы", "*.*")])
        if paths:
            self.add_paths(list(paths))

    def pick_mygames(self):
        if not os.path.isdir(GAMES_DIR):
            messagebox.showinfo(
                "Папка не найдена",
                "Рядом должна быть папка «Мои_игры_класть_сюда».\n"
                "Положи в неё игры и нажми снова — или выбери файлы кнопкой слева.")
            return
        exts = (".gb", ".gbc", ".sgb", ".rom", ".bin")
        paths = [os.path.join(GAMES_DIR, f) for f in sorted(os.listdir(GAMES_DIR))
                 if f.lower().endswith(exts)]
        if not paths:
            messagebox.showinfo(
                "Папка пустая",
                "В папке «Мои_игры_класть_сюда» пока нет игр.\n"
                "Скопируй туда файлы .gb / .gbc и нажми снова.")
            return
        self.add_paths(paths)

    def remove_sel(self):
        # curselection() отдаёт строки — в int() обязательно
        for idx in reversed(self.listbox.curselection()):
            del self.infos[int(idx)]
        self.refit()

    def clear_all(self):
        self.infos = []
        self.refit()

    # ---------- расклад / сборка ----------
    def refit(self):
        """Пересчитать, что влезает, и перерисовать список."""
        self.listbox.delete(0, "end")
        if not self.infos:
            self.listbox.insert("end", "(пока пусто — нажми «ВЫБРАТЬ ИГРЫ») ")
            self.cap_label.config(text="Место: 0 из %d КБ" % CAP_KIB)
            self.bar["value"] = 0
            return
        _blob, _table, placed, skipped, used = pack.build(self.infos)
        ok_ids = {id(i) for i in placed}
        why = {id(i): w for i, w in skipped}
        for inf in self.infos:
            if id(inf) in ok_ids:
                mark = "✓"
                tail = ""
                if inf.sram:
                    tail = "  (сейвы работают 💾)"
                self.listbox.insert(
                    "end", "%s %-22s %4d КБ%s" % (mark, inf.name,
                                                  inf.size // 1024, tail))
            else:
                self.listbox.insert(
                    "end", "✗ %-22s %4d КБ — %s" % (
                        inf.name, inf.size // 1024,
                        kid_reason(why.get(id(inf), "?"))))
        free = pack.CAPACITY - used
        self.cap_label.config(
            text="Место: %d из %d КБ (свободно %d)" % (
                used // 1024, CAP_KIB, free // 1024))
        self.bar["value"] = used * 100.0 / pack.CAPACITY

    def build(self):
        if not self.infos:
            messagebox.showwarning("Нечего собирать",
                                   "Сначала выбери игры (шаг ①).")
            return
        _blob, table, placed, skipped, used = pack.build(self.infos)
        if not placed:
            messagebox.showerror(
                "Ничего не влезло",
                "Ни одна игра не помещается в консоль.\n\n" +
                "\n".join("%s (%d КБ) — %s" % (i.name, i.size // 1024,
                                                kid_reason(w))
                           for i, w in skipped))
            return
        blocks = [(pack.TABLE_OFF, table)]
        for inf in placed:
            blocks.append((inf.off, inf.data))
        n = pack.write_uf2(blocks, OUT_FILE)
        lines = ["✅ ГОТОВО! Собрано игр: %d, занято %d из %d КБ." % (
            len(placed), used // 1024, CAP_KIB),
            "Файл: %s (%d КБ)" % (os.path.basename(OUT_FILE), n * 512 // 1024),
            "Теперь шаг ③: перетащи его на диск RPI-RP2."]
        if skipped:
            lines.append("")
            lines.append("⚠ НЕ ВЛЕЗЛО игр: %d — в консоли их НЕ будет:" % len(skipped))
            for inf, why in skipped:
                lines.append("• %s (%d КБ) — %s" % (
                    inf.name, inf.size // 1024, kid_reason(why)))
                if why.startswith("не влезло по месту"):
                    need = (inf.size + pack.ALIGN - 1) // pack.ALIGN * pack.ALIGN
                    deficit = need - (pack.CAPACITY - used)
                    if deficit > 0:
                        lines.append("    чтобы вошла — освободи %d КБ "
                                     "(убери что-нибудь из списка)" % kib(deficit))
        self.log("\n".join(lines))
        messagebox.showinfo("Готово",
                            "Файл games.uf2 собран!\n\n"
                            "Игр внутри: %d\n%s" % (
                                len(placed),
                                "⚠ %d не влезло (подробности в окне)." % len(skipped)
                                if skipped else "Влезло всё ✅") +
                            "\n\nТеперь перетащи games.uf2 на диск RPI-RP2.")
        self.open_out_dir()

    def open_out_dir(self):
        try:
            if sys.platform.startswith("win"):
                os.startfile(HERE)                       # noqa: S606 — своя папка
            elif sys.platform == "darwin":
                os.system('open "%s"' % HERE)
            else:
                os.system('xdg-open "%s" &' % HERE)
        except Exception as ex:
            messagebox.showwarning("Не открылось",
                                   "Открой папку вручную:\n%s\n\n(%s)" % (HERE, ex))


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
