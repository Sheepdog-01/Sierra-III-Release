"""The installer's look: deep blue with a cyan accent, drawn by Tk itself.
No image, logo or font files, and nothing taken from the game or from
Sierra-III's own art."""
import tkinter as tk
from tkinter import ttk

BG = '#08111f'
PANEL = '#111f32'
FIELD = '#0b1627'
TEXT = '#f1f7ff'
MUTED = '#b4c8df'
DIM = '#5f7590'
ACCENT = '#67dcff'
BLUE = '#155d9e'
BORDER = '#294665'
ERROR = '#ffb8a8'
SUCCESS = '#87e0be'
FONT = 'Segoe UI'


def apply(root):
    root.configure(background=BG)
    style = ttk.Style(root)
    style.theme_use('clam')
    # the fields' own font (a font for every widget would override the
    # labels' styles)
    for widget in ('*TEntry*Font', '*TCombobox*Font', '*Listbox*Font'):
        root.option_add(widget, '{%s} 11' % FONT)
    style.configure('.', background=BG, foreground=TEXT, font=(FONT, 11))
    style.configure('TFrame', background=BG)
    style.configure('Card.TFrame', background=PANEL)
    style.configure('TLabel', background=BG, foreground=TEXT)
    style.configure('Muted.TLabel', foreground=MUTED)
    style.configure('Heading.TLabel', font=(FONT, 18, 'bold'), foreground=TEXT)
    style.configure('Step.TLabel', font=(FONT, 11, 'bold'), foreground=ACCENT)
    style.configure('Card.TLabel', background=PANEL)
    style.configure('CardMuted.TLabel', background=PANEL, foreground=MUTED)
    style.configure('Error.TLabel', foreground=ERROR)
    style.configure('Success.TLabel', foreground=SUCCESS)
    style.configure('Legal.TLabel', foreground=MUTED, font=(FONT, 9))
    style.configure('TButton', background=PANEL, foreground=TEXT, padding=(15, 9), bordercolor=BORDER,
                    focusthickness=2, focuscolor=ACCENT)
    style.map('TButton', background=[('active', '#244565'), ('disabled', BG)],
              foreground=[('disabled', DIM)])
    style.configure('Primary.TButton', background=BLUE, foreground=TEXT, font=(FONT, 11, 'bold'), padding=(24, 10))
    style.map('Primary.TButton', background=[('active', '#2378ba'), ('disabled', '#20344b')])
    style.configure('Compact.TButton', padding=(12, 5))
    style.configure('TEntry', fieldbackground=FIELD, foreground=TEXT, insertcolor=ACCENT, bordercolor=BORDER,
                    padding=7)
    style.configure('TCombobox', fieldbackground=FIELD, foreground=TEXT, background=PANEL, arrowcolor=ACCENT,
                    padding=7)
    style.map('TCombobox', fieldbackground=[('readonly', FIELD)], selectbackground=[('readonly', BLUE)],
              selectforeground=[('readonly', TEXT)])
    root.option_add('*TCombobox*Listbox.background', FIELD)
    root.option_add('*TCombobox*Listbox.foreground', TEXT)
    style.configure('TCheckbutton', background=BG, foreground=TEXT, padding=4, indicatorbackground=FIELD,
                    indicatorforeground=ACCENT, indicatormargin=(0, 0, 8, 0), bordercolor=ACCENT, upperbordercolor=ACCENT,
                    lowerbordercolor=ACCENT)
    style.map('TCheckbutton', background=[('active', BG)], indicatorbackground=[('selected', BLUE), ('disabled', BG)],
              foreground=[('disabled', DIM)])
    style.configure('Horizontal.TProgressbar', troughcolor=FIELD, background=ACCENT, bordercolor=BORDER,
                    lightcolor=ACCENT, darkcolor=ACCENT, thickness=8)
    return style


class Banner(tk.Canvas):
    """the header: the title, and a segmented orbital arc drawn with lines"""

    def __init__(self, parent, title, subtitle):
        super().__init__(parent, background=PANEL, height=104, highlightthickness=0)
        self.title, self.subtitle = title, subtitle
        self.bind('<Configure>', self.draw)

    def draw(self, event):
        self.delete('all')
        w = event.width
        self.create_rectangle(0, 101, w, 104, fill=ACCENT, outline='')
        for inset, color in ((0, BORDER), (13, BLUE), (23, ACCENT)):
            self.create_arc(w - 230 + inset, -120 + inset, w + 60 - inset, 170 - inset, start=40, extent=235,
                            style='arc', width=3, outline=color)
        for n in range(6):
            x = w - 330 + n * 16
            self.create_line(x, 76, x + 9, 76, fill=ACCENT if n < 4 else BORDER, width=3)
        self.create_text(28, 38, anchor='w', text=self.title, fill=TEXT, font=(FONT, 26, 'bold'))
        self.create_text(30, 74, anchor='w', text=self.subtitle, fill=ACCENT, font=(FONT, 11, 'bold'))


class Steps(tk.Canvas):
    """the step rail down the left: done, now and to come"""

    def __init__(self, parent, names):
        super().__init__(parent, background=BG, width=210, highlightthickness=0)
        self.names, self.now, self.done = names, 0, set()
        self.bind('<Configure>', lambda event: self.draw())

    def show(self, now, done):
        self.now, self.done = now, set(done)
        self.draw()

    def draw(self):
        self.delete('all')
        top, gap = 24, 62
        self.create_line(36, top, 36, top + gap * (len(self.names) - 1), fill=BORDER, width=2)
        for n, name in enumerate(self.names):
            y = top + n * gap
            if n in self.done:
                fill, ring, mark, colour = BLUE, ACCENT, '✓', TEXT
            elif n == self.now:
                fill, ring, mark, colour = ACCENT, ACCENT, str(n + 1), BG
            else:
                fill, ring, mark, colour = PANEL, BORDER, str(n + 1), MUTED
            self.create_oval(22, y - 14, 50, y + 14, fill=fill, outline=ring, width=2)
            self.create_text(36, y, text=mark, fill=colour, font=(FONT, 11, 'bold'))
            self.create_text(64, y, anchor='w', text=name, font=(FONT, 11, 'bold' if n == self.now else 'normal'),
                             fill=TEXT if n == self.now else (MUTED if n in self.done else DIM))


class Choice(tk.Frame):
    """a large selectable card (the game sources); off when not ready, with a
    NOT YET badge"""

    def __init__(self, parent, title, detail, ready, variable, value):
        super().__init__(parent, background=PANEL, highlightthickness=2, highlightbackground=BORDER, cursor='hand2'
                         if ready else 'arrow', padx=16, pady=12)
        self.variable, self.value, self.ready = variable, value, ready
        top = tk.Frame(self, background=PANEL)
        top.pack(fill='x')
        self.dot = tk.Canvas(top, width=22, height=22, background=PANEL, highlightthickness=0)
        self.dot.pack(side='left', padx=(0, 10))
        tk.Label(top, text=title, background=PANEL, foreground=TEXT if ready else DIM,
                 font=(FONT, 12, 'bold')).pack(side='left')
        if not ready:
            tk.Label(top, text=' NOT YET ', background=BORDER, foreground=ACCENT, font=(FONT, 8, 'bold')).pack(
                side='left', padx=10)
        tk.Label(self, text=detail, background=PANEL, foreground=MUTED if ready else DIM, font=(FONT, 10),
                 anchor='w', justify='left', wraplength=520).pack(fill='x', padx=(32, 0), pady=(4, 0))
        if ready:
            for widget in (self, top, self.dot, *top.winfo_children(), *self.winfo_children()):
                widget.bind('<Button-1>', lambda _: variable.set(value))
        variable.trace_add('write', lambda *_: self.draw())
        self.draw()

    def draw(self):
        on = self.ready and self.variable.get() == self.value
        self.configure(highlightbackground=ACCENT if on else BORDER)
        self.dot.delete('all')
        self.dot.create_oval(3, 3, 19, 19, outline=ACCENT if self.ready else DIM, width=2)
        if on:
            self.dot.create_oval(7, 7, 15, 15, fill=ACCENT, outline='')
