"""The installer window: five steps, one page each, from choosing the game to
a Memory Stick ready to play. The work itself is in core.py; this file only
asks, shows and waits. Everything slow runs on a worker thread, which talks
to the window through a queue and never touches Tk."""
import os
import queue
import threading
import tkinter as tk
from tkinter import filedialog, ttk

from . import core, theme

TITLE = 'Sierra-III'
STEPS = ['Your game', 'Your files', 'Bake the maps', 'Memory Stick', 'Play']
BAKE_STAGES = ['Reading your disc', 'Setting up (the first time only)', 'Baking the maps', 'Putting it together']
NOTICE = ('Unofficial and free. Not endorsed by or affiliated with Microsoft, Halo Studios, Bungie or Sony. Halo '
          'is a trademark of Microsoft Corporation. PSP and PlayStation are trademarks of Sony Interactive '
          'Entertainment Inc.; Memory Stick is a trademark of Sony Group Corporation. Nothing from the game comes with this installer: it uses your own disc, and '
          'what it bakes stays on your computer and your PSP.')


class Installer:
    def __init__(self, root):
        self.root = root
        root.title(TITLE + ' Installer')
        root.geometry('1000x700')
        root.minsize(860, 640)
        theme.apply(root)
        theme.Banner(root, TITLE, 'INSTALLER').pack(fill='x')
        self.events = queue.Queue(maxsize=1024)
        self.ctx = None
        self.page = 0
        self.done = set()
        self.baked = None   # check_baked's report, once there is something to install
        self.source = tk.StringVar(value='xbox')
        self.owned = tk.BooleanVar(value=False)
        self.disc = tk.StringVar()
        self.zip = tk.StringVar()
        self.use_baked = tk.BooleanVar(value=False)
        self.baked_folder = tk.StringVar()
        self.stick = tk.StringVar()
        self.sticks = []

        footer = ttk.Frame(root, padding=(28, 6, 28, 12))
        footer.pack(side='bottom', fill='x')
        middle = ttk.Frame(root)
        middle.pack(fill='both', expand=True)
        self.rail = theme.Steps(middle, STEPS)
        self.rail.pack(side='left', fill='y', pady=(18, 0))
        self.body = ttk.Frame(middle, padding=(8, 22, 28, 8))
        self.body.pack(side='left', fill='both', expand=True)

        self.status = tk.StringVar()
        self.status_label = ttk.Label(footer, textvariable=self.status, wraplength=900)
        self.status_label.pack(anchor='w', pady=(0, 8))
        buttons = ttk.Frame(footer)
        buttons.pack(fill='x')
        self.back = ttk.Button(buttons, text='←  Back', command=lambda: self.show(self.page - 1))
        self.back.pack(side='left')
        self.next = ttk.Button(buttons, text='Next  →', style='Primary.TButton', command=self.forward)
        self.next.pack(side='right')
        self.cancel = ttk.Button(buttons, text='Stop', command=self.stop)
        ttk.Label(footer, text=NOTICE, style='Legal.TLabel', wraplength=940).pack(anchor='w', pady=(10, 0))
        root.bind('<Configure>', lambda event: self.status_label.configure(wraplength=max(300, event.width - 80))
                  if event.widget is root else None, add='+')

        self.pages = [self.game_page(), self.files_page(), self.bake_page(), self.stick_page(), self.play_page()]
        root.protocol('WM_DELETE_WINDOW', self.close)
        self.show(0)
        self.poll()

    # --- the pages ----------------------------------------------------------

    def heading(self, parent, title, text):
        ttk.Label(parent, text=title, style='Heading.TLabel').pack(anchor='w')
        ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=700).pack(anchor='w', pady=(4, 16))

    def game_page(self):
        page = ttk.Frame(self.body)
        self.heading(page, 'Which game are you installing from?',
                     'The installer bakes the PSP\'s maps from your own copy of the game. Nothing from the game '
                     'comes with it.')
        for key, source in core.SOURCES.items():
            theme.Choice(page, source['title'], source['detail'], source['ready'], self.source, key).pack(
                fill='x', pady=(0, 10))
        ttk.Checkbutton(page, text='I own this game, and I made the disc image from my own disc.',
                        variable=self.owned, command=self.refresh).pack(anchor='w', pady=(8, 0))
        return page

    def path_row(self, parent, label, hint, variable, kinds):
        ttk.Label(parent, text=label, style='Step.TLabel').pack(anchor='w', pady=(10, 2))
        ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=700).pack(anchor='w', pady=(0, 5))
        row = ttk.Frame(parent)
        row.pack(fill='x')
        ttk.Entry(row, textvariable=variable).pack(side='left', fill='x', expand=True)
        for kind, types in kinds:
            def pick(kind=kind, types=types):
                chosen = filedialog.askdirectory(parent=self.root, title=label) if kind == 'folder' else \
                    filedialog.askopenfilename(parent=self.root, title=label, filetypes=types)
                if chosen:
                    variable.set(os.path.normpath(chosen))
            ttk.Button(row, text='Choose ' + kind, style='Compact.TButton', command=pick).pack(side='left',
                                                                                              padx=(8, 0))
        return row

    def files_page(self):
        page = ttk.Frame(self.body)
        self.heading(page, 'Your two files', 'Your disc image, and the release zip from the same release as this '
                     'bake bundle. The bake checks every map against the retail disc before it starts.')
        self.path_row(page, 'Your disc image', 'An image of your disc (usually .iso), or a folder of the disc\'s '
                      'files (the one holding default.xbe and maps).', self.disc,
                      [('file', [('Disc images', '*.iso *.xiso'), ('All files', '*.*')]), ('folder', None)])
        self.path_row(page, 'The release zip', 'halo_psp_bNNN.zip for the campaign, or halo_psp_bNNNmp.zip for '
                      'the multiplayer build, as downloaded.', self.zip, [('file', [('Release zips', '*.zip')])])
        ttk.Checkbutton(page, text='I already baked with "Bake Halo": use its COPY-TO-MEMORY-STICK folder instead',
                        variable=self.use_baked, command=self.refresh).pack(anchor='w', pady=(14, 0))
        # shown only with the box ticked
        self.baked_box = ttk.Frame(page)
        self.path_row(self.baked_box, 'The baked folder', 'COPY-TO-MEMORY-STICK, in the bake bundle\'s folder.',
                      self.baked_folder, [('folder', None)])
        return page

    def bake_page(self):
        page = ttk.Frame(self.body)
        self.heading(page, 'Bake the maps', 'About ten minutes on an 8-core PC, longer on fewer cores; a few '
                     'minutes when you bake again. The first time it fetches the bake\'s Python libraries (about '
                     '30 MB). Leave the window open.')
        card = ttk.Frame(page, style='Card.TFrame', padding=(16, 12))
        card.pack(fill='x')
        self.stage_marks = []
        for number, name in enumerate(BAKE_STAGES, 1):
            row = ttk.Frame(card, style='Card.TFrame')
            row.pack(fill='x', pady=3)
            mark = tk.Canvas(row, width=20, height=20, background=theme.PANEL, highlightthickness=0)
            mark.pack(side='left', padx=(0, 10))
            ttk.Label(row, text='%d/4  %s' % (number, name), style='CardMuted.TLabel').pack(side='left')
            self.stage_marks.append(mark)
        self.bake_progress = ttk.Progressbar(page, mode='determinate', maximum=1.0)
        self.bake_progress.pack(fill='x', pady=(14, 8))
        self.log = tk.Text(page, height=5, background=theme.FIELD, foreground=theme.MUTED, relief='flat',
                           wrap='word', padx=10, pady=6, font=(theme.FONT, 9), state='disabled')
        self.log.pack(fill='both', expand=True)
        self.mark_stages(0)
        return page

    def stick_page(self):
        page = ttk.Frame(self.body)
        self.heading(page, 'Copy to your Memory Stick', 'Put the PSP\'s Memory Stick in this computer (a card '
                     'reader, or the PSP in USB mode). Only removable drives that look like a PSP Memory Stick are '
                     'listed.')
        row = ttk.Frame(page)
        row.pack(fill='x')
        self.stick_box = ttk.Combobox(row, textvariable=self.stick, state='readonly', width=44)
        self.stick_box.pack(side='left')
        ttk.Button(row, text='Refresh', style='Compact.TButton', command=self.find_sticks).pack(side='left',
                                                                                               padx=(8, 0))
        card = ttk.Frame(page, style='Card.TFrame', padding=(16, 12))
        card.pack(fill='x', pady=(16, 0))
        ttk.Label(card, text='WHAT IT WRITES', style='Step.TLabel', background=theme.PANEL).pack(anchor='w')
        self.writes = tk.StringVar()
        ttk.Label(card, textvariable=self.writes, style='CardMuted.TLabel', wraplength=640, justify='left').pack(
            anchor='w', pady=(6, 0))
        self.copy_progress = ttk.Progressbar(page, mode='determinate', maximum=4.0)
        self.copy_progress.pack(fill='x', pady=(16, 0))
        return page

    def play_page(self):
        page = ttk.Frame(self.body)
        self.heading(page, 'Ready to play', 'Every file was read back and checked.')
        card = ttk.Frame(page, style='Card.TFrame', padding=(16, 12))
        card.pack(fill='x')
        self.played = tk.StringVar()
        ttk.Label(card, textvariable=self.played, style='CardMuted.TLabel', wraplength=640, justify='left').pack(
            anchor='w')
        return page

    # --- moving between pages -------------------------------------------------

    def show(self, page):
        if self.ctx or not 0 <= page < len(self.pages):
            return
        for frame in self.pages:
            frame.pack_forget()
        self.page = page
        self.pages[page].pack(fill='both', expand=True)
        if page == 3:
            self.find_sticks()
        self.say({0: 'Choose where the game comes from.', 1: 'Choose your two files, then Next.',
                  2: 'Start the bake when you\'re ready.', 3: 'Choose the Memory Stick, then Install.',
                  4: 'All done.'}[page])
        self.refresh()

    def refresh(self):
        self.rail.show(self.page, self.done)
        busy = self.ctx is not None
        if self.use_baked.get():
            self.baked_box.pack(fill='x')
        else:
            self.baked_box.pack_forget()
        labels = {0: 'Next  →', 1: 'Next  →', 2: 'Start the bake' if not self.baked else 'Next  →',
                  3: 'Install', 4: 'Close'}
        self.next.configure(text=labels[self.page], state='disabled' if busy or (
            self.page == 0 and not (self.owned.get() and core.SOURCES[self.source.get()]['ready'])) else 'normal')
        self.back.configure(state='disabled' if busy or self.page in (0, 4) else 'normal')
        if busy:
            self.cancel.pack(side='right', padx=(0, 8))
        else:
            self.cancel.pack_forget()
        if self.page == 3 and self.baked:
            self.writes.set('PSP/GAME/%s/  the game, merged with what is there\nhalo/maps/  your %d baked maps '
                            '(an older halo/maps is replaced)\nhalo/  the game\'s files beside them; saves already '
                            'there are kept\n\nAbout %s. ARK-4, SEPLUGINS and other games are never touched.'
                            % (self.baked['game'], self.baked['maps'], core.size_text(self.baked['bytes'])))

    def forward(self):
        if self.page == 0:
            self.done.add(0)
            self.show(1)
        elif self.page == 1:
            try:
                if self.use_baked.get():
                    self.baked = core.check_baked(self.baked_folder.get())
                    self.done |= {1, 2}
                    self.show(3)
                    return
                core.check_disc(self.disc.get())
                core.check_zip(self.zip.get())
            except core.InstallError as e:
                return self.say(str(e), 'error')
            self.baked = None
            self.done.add(1)
            self.show(2)
        elif self.page == 2:
            if self.baked:
                return self.show(3)
            # every Tk value read here, on the window's thread
            disc, game_zip = self.disc.get(), self.zip.get()
            self.run('bake', lambda ctx: core.bake(disc, game_zip, ctx))
        elif self.page == 3:
            index = self.stick_box.current()
            if index < 0:
                return self.say('Choose your PSP\'s Memory Stick.', 'error')
            root, folder = self.sticks[index]['root'], self.baked['folder']
            self.run('install', lambda ctx: core.install(folder, root, ctx))
        else:
            self.close()

    def find_sticks(self):
        self.sticks = found = core.find_sticks()
        self.stick_box.configure(values=['%s  (%s free)' % (s['root'], core.size_text(s['free'])) for s in found])
        self.stick.set('')
        if len(found) == 1:
            self.stick_box.current(0)
        elif found:
            self.say('%d drives look like a PSP Memory Stick: choose yours.' % len(found))
        else:
            self.say('No PSP Memory Stick found: put it in, then Refresh.')

    # --- the work ---------------------------------------------------------------

    def run(self, action, work):
        if self.ctx:
            return
        self.ctx = core.Context(self.emit)
        ctx = self.ctx
        if action == 'bake':
            self.mark_stages(0)
            self.bake_progress.configure(value=0)
        self.say('Baking: leave this window open.' if action == 'bake' else 'Copying to the Memory Stick ...')
        self.refresh()

        def worker():
            try:
                result = ('success', work(ctx))
            except core.Cancelled as e:
                result = ('cancelled', str(e))
            except (core.InstallError, OSError) as e:
                result = ('error', core.home(str(e)))
            except Exception as e:
                # whatever it was, the window hears that the step ended
                result = ('error', 'Something went wrong that the installer didn\'t expect (%s: %s).'
                          % (type(e).__name__, core.home(str(e))))
            self.events.put(('done', action, result))
        threading.Thread(target=worker, daemon=True).start()

    def emit(self, kind, **data):
        try:
            self.events.put_nowait((kind, None, data))
        except queue.Full:
            pass

    def poll(self):
        while True:
            try:
                kind, action, data = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == 'log':
                self.append(data['text'])
            elif kind == 'stage' and self.page == 2:
                self.mark_stages(data['number'], data['fraction'])
            elif kind == 'stage':
                self.copy_progress.configure(value=data['number'] - 1 + (data['fraction'] or 1.0))
            elif kind == 'done':
                self.finish(action, *data)
        self.root.after(100, self.poll)

    def finish(self, action, result, value):
        self.ctx = None
        if result != 'success':
            self.say(value, 'error' if result == 'error' else None)
            self.refresh()
            return
        if action == 'bake':
            self.baked = value
            self.mark_stages(5)
            self.done.add(2)
            self.say('Baked %d maps. Next: copy them to your Memory Stick.' % value['maps'], 'success')
            self.refresh()
        else:
            self.done |= {3, 4}
            self.played.set('Installed %d files: %s and its data.\n\nEject the Memory Stick safely before you '
                            'unplug it. Then on the PSP: Game, then Memory Stick, then the game.\n\nYour disc '
                            'image and the baked maps are yours alone: keep them to yourself.'
                            % (value['files'], value['folder']))
            self.show(4)

    def mark_stages(self, now, fraction=None):
        """stages before now done, now running"""
        for number, mark in enumerate(self.stage_marks, 1):
            mark.delete('all')
            if number < now:
                mark.create_oval(2, 2, 18, 18, fill=theme.BLUE, outline=theme.ACCENT)
                mark.create_text(10, 10, text='✓', fill=theme.TEXT, font=(theme.FONT, 8, 'bold'))
            elif number == now:
                mark.create_oval(2, 2, 18, 18, fill=theme.ACCENT, outline=theme.ACCENT)
            else:
                mark.create_oval(2, 2, 18, 18, outline=theme.BORDER, width=2)
        if now:
            # never backwards within a bake (a stage's last line has no fraction)
            value = min(1.0, (min(now, 5) - 1 + (fraction or 0)) / 4)
            self.bake_progress.configure(value=max(value, float(self.bake_progress['value'])))

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text + '\n')
        if int(self.log.index('end-1c').split('.')[0]) > 400:
            self.log.delete('1.0', '100.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def say(self, text, kind=None):
        self.status.set(text)
        self.status_label.configure(style={'error': 'Error.TLabel', 'success': 'Success.TLabel'}.get(kind, 'TLabel'))

    def stop(self):
        if self.ctx:
            # stopping the bake can take a few seconds: not on the window's thread
            threading.Thread(target=self.ctx.cancel, daemon=True).start()
            self.say('Stopping safely ...')

    def close(self):
        if self.ctx:
            self.stop()
            self.say('Stopping safely: close the window again once it has stopped.')
        else:
            self.root.destroy()


def main():
    try:
        root = tk.Tk()
    except tk.TclError as e:
        print('The installer window can\'t open here (%s). "Bake Halo" bakes without a window; then copy '
              'COPY-TO-MEMORY-STICK to the stick as the README says.' % e)
        return 1
    Installer(root)
    root.mainloop()
    return 0
