# ZERO·KRISTAL 🎵🤖

Lecteur audio façon **Winamp**, écrit en **C++ / Qt6**.

Une fenêtre sans bordure, un spectre animé en temps réel (FFT), un égaliseur 10 bandes qui filtre *vraiment* le son, la recherche YouTube, les web radios, les playlists — et un visualiseur ASCII custom

## Fonctionnalités

- 🎨 **Visualiseurs** (bouton `🎨`) : *Barres*, *Points*, *Onde* (oscilloscope), *Cristal/robot* (tête de robot + oscillation vocale gauche/droite)
- 🎚️ **Égaliseur 10 bandes** (biquad, appliqué au signal) + presets
- 🔍 **Recherche YouTube** intégrée (via `yt-dlp`)
- 📻 **Web radios** — liste lisible depuis `~/.config/zer0-kristal/radios.txt`, bouton `➕` pour en ajouter
- 💾 **Playlists** — créer / charger / supprimer / ajouter la piste courante (`~/.config/zer0-kristal/playlists/`)
- 📂 Ouverture de **fichiers** ou d'un **dossier entier** (scan récursif)
- 🔗 Lecture d'**URL / streams** (radio, flux direct, YouTube)
- 🔀 **Shuffle** · 🔁 **Boucle** (off / toutes / une)
- ▶ **Piste suivante automatique** à la fin d'une piste
- 🎨 Thème sombre néon (Catppuccin Mocha + accents néon)

## Installation

### Clone du repository

```bash
git clone https://github.com/Richerrail/zer0-kristal.git
cd zer0-kristal
```

## Dépendances

| Paquet | Rôle |
|--------|------|
| `qt6-base` | Qt Widgets |
| `qt6-multimedia` | Sortie audio (`QAudioSink`) |
| `ffmpeg` | Décodage audio (MP3/FLAC/OGG/streams…) |
| `ffprobe` | Durée des pistes |
| `yt-dlp` | Recherche + résolution YouTube |

**Installation (Arch / Omarchy)** :
```bash
sudo pacman -S qt6-base qt6-multimedia ffmpeg yt-dlp
```

## Compilation

```bash
./build.sh
# ou manuellement :
g++ -O2 -fPIC main.cpp -o zer0-kristal $(pkg-config --cflags --libs Qt6Widgets Qt6Multimedia)
```

## Utilisation

```bash
./zer0-kristal                          # ouvre l'app
./zer0-kristal "URL_ou_fichier"         # lance direct un fichier/stream/radio
```

### Raccourcis / contrôles
- Boutons : ⏮ ▶ ⏹ ⏭ + 🔀 shuffle + 🔁 boucle + volume + seek
- `🎨` change le visualiseur
- Champ de recherche : tape + **Entrée** → résultats YouTube dans la playlist, double-clic pour jouer

## Fichiers de config

```
~/.config/zer0-kristal/radios.txt       # "Nom|URL" — une radio par ligne
~/.config/zer0-kristal/playlists/*.txt  # une URL par ligne
```

## Architecture

```
main.cpp  →  tout le projet (un seul fichier, simple à lire)
  ├─ fft()               FFT radix-2 (spectre)
  ├─ Equalizer/Biquad    10 bandes peaking (RBJ), traitement audio réel
  ├─ AudioEngine         ffmpeg → PCM → EQ → FIFO (drainée à vitesse réelle) → QAudioSink
  ├─ SpectrumWidget      rendu QPainter (barres / points / onde / robot)
  └─ MainWindow          fenêtre frameless + contrôles + playlist + égaliseur
```

Le pipeline audio : `ffmpeg` (décode tout) → égaliseur biquad → FFT (spectre live) → `QAudioSink` (son). Le visualiseur affiche le **vrai** spectre de ce qui sort, pas un faux aléatoire.

## Licence

MIT License

Copyright (c) 2026 Richerrail

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
