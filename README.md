# ZERO·KRISTAL 🎵🤖

Lecteur audio façon **Winamp**, écrit en **C++ / Qt6**.

Une fenêtre sans bordure, un spectre animé en temps réel (FFT), un égaliseur 10 bandes qui filtre *vraiment* le son, la recherche YouTube, les web radios, les playlists — et une tête de robot custom au centre du visualiseur.

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

Choisis la tienne (MIT recommandé). À toi de voir.