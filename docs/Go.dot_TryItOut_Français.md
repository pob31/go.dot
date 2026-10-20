# Go.dot — Pour l'essayer

Oct 15, 2026 · @Pierre-Olivier Boulant

## Go.dot, Ce que c'est ?

Go.dot est un logiciel de conduite de spectacle libre et gratuit (GPL-3), pour le théâtre, la danse et la musique live. Vous construisez une liste de cues, vous appuyez sur GO, et il joue le son et la vidéo, les fait monter ou descendre, les boucle, les envoie aux bonnes sorties et pilote le reste de l'installation. Il tourne sous Windows, macOS et Linux.

Il emprunte à trois outils que beaucoup d'entre vous connaissent :

- **QLab** pour la liste de cues, les fondus, les enchaînements et les habitudes du régisseur.
- **Ableton Live** pour garder la main sur les paramètres pendant que le son tourne.
- **Chataigne** pour transformer les données de contrôle entrantes en actions.

Ce n'est ni un banc de montage audionumérique, ni une console lumière, ni un moteur de spatialisation. Il conduit le spectacle et dirige les machines qui font ces métiers.

Trois engagements façonnent tout le reste :

- **GO ne bloque jamais.** Quoi qu'il se passe, la cue suivante part quand vous appuyez.
- **Trois façons d'arrêter.** Échap arrête proprement, comme si les cues s'étaient terminées normalement. Échap deux fois coupe immédiatement tout ce que Go.dot produit. **Doh!** rattrape l'erreur la plus courante : un GO parti trop tôt.
- **Rien ne se perd.** Chaque modification peut être annulée, et le spectacle est sauvegardé en continu : un plantage ne vous coûte rien.

Ceci est une **première version** (0.1), prête à essayer en salle de répétition ou chez soi, et pas encore pour une première.

L'interface du logiciel est en anglais ; les noms des menus et des boutons sont donc cités en anglais ci-dessous.

## Installer et lancer

Téléchargez la dernière version pour votre système sur la [page des versions](https://github.com/pob31/go.dot/releases), installez-la et lancez Go.dot. Il ouvre un spectacle vide, *Untitled*, sur votre interface audio par défaut, avec les réglages du spectacle ouverts pour en choisir une autre.

| Système | Téléchargement | Premier lancement | Journal pour les rapports de bug |
| --- | --- | --- | --- |
| Windows 10/11 | `-setup.exe` (installateur : menu Démarrer, ouverture des `.wfg` par double-clic) ou `.zip` (à décompresser où vous voulez, lancer `Go.dot.exe`) | Pas encore signé : SmartScreen avertit, choisir *Informations complémentaires* puis *Exécuter quand même* | `%APPDATA%\Go.dot\logs` |
| macOS 13.3+ (Apple silicon et Intel) | `.dmg`, glisser Go.dot dans Applications | Signé et notarié, s'ouvre sans avertissement. Autoriser l'accès au micro pour les cues micro | `~/Library/Logs/Go.dot` |
| Linux (compilé sur Ubuntu 24.04) | `.deb` (`sudo apt install ./go.dot-…deb`) ou archive tar (lancer `./go.dot.sh`) | Demande ALSA, FreeType, fontconfig, X11 (présents sur tout bureau), et curl ou wget pour chercher une version plus récente | `~/.local/state/Go.dot/logs` |

**Sous Linux avec une interface multicanal**, PipeWire ne donne que deux canaux via ALSA. Installez `pipewire-jack`, passez le profil de l'interface en *Pro Audio* dans pavucontrol, et choisissez JACK dans les réglages du spectacle.

**Sous Windows**, les pilotes ASIO sont pris en charge et sont le meilleur choix pour une interface pro.

Quelques vérifications depuis un terminal (sous Windows, `wfg.exe` dans le dossier d'installation ; sous macOS, dans `Go.dot.app/Contents/MacOS/`) :

- `wfg --version` — quelle version vous avez
- `wfg devices` — les interfaces audio utilisables
- `wfg midi` — les ports MIDI visibles
- `wfg plugins --scan` — trouver vos plugins VST3, AU et LV2
- `wfg validate <spectacle>` — vérifier un dossier de spectacle et lister chaque problème

**Nouvelles versions.** Une fois par jour, au lancement, Go.dot regarde sur GitHub si une version plus récente est sortie et le dit sur sa ligne de titre : un appui montre les nouveautés, ou ne signale plus que la suivante. Rien n'est téléchargé : installez la nouvelle version comme celle-ci. *Help > About Go.dot* dit quelle version vous avez, *Help > Look for a newer Go.dot now* regarde tout de suite, et *Help > Look for a newer Go.dot at launch* coupe ce regard quotidien, pour un ordinateur de spectacle gardé hors ligne.

*Show settings* range ses onglets à gauche en cinq sections : Show, Audio, Video, Controllers et Getting started. La dernière, **Getting started**, reprend chaque autre onglet sous sa section, avec une phrase sur son rôle.

## Votre premier spectacle en dix minutes

Un spectacle est un dossier avec au moins un fichier `.wfg` et un dossier `media/` où Go.dot copie vos sons. Rangez-le où vous voulez, emportez-le sur une autre machine : seuls les réglages audio sont à revoir.

1. **Choisir l'interface audio.** *Show > Show settings… > Audio interface*. Le bouton GO passe au jaune quand l'audio tourne.
2. **Créer et nommer les sorties** (canaux de mixage ou sorties directes, mono ou stéréo). Dans *Outputs*, créez les sorties du spectacle (« Face G/D », « Sub », « Retours »). Dans *Output patch*, indiquez sur quel canal physique chacune arrive. Les cues visent des noms : changer de salle, c'est changer de patch, sans toucher aux cues.
3. **Ajouter des sons.** Glissez des fichiers WAV, AIFF, FLAC ou Ogg sur la liste. Chacun devient une cue média au nom du fichier. Déposez-en un sur une cue existante pour remplacer son fichier.
4. **Ajouter d'autres cues** depuis la rangée *Add* au-dessus de la liste : `memo`, `media`, `mic`, `video`, `fade`, `transport`, `osc`, `midi`, `process`, `script`, `group`. Ceux qui portent un ▾ ouvrent un petit menu de variantes.
5. **Régler chaque cue** dans l'inspecteur à droite : nom, pre-wait, post-wait, enchaînement, niveau. Les boutons en haut de l'inspecteur ouvrent les panneaux en pied de fenêtre : *Waveform*, *EQ*, *FX*, *Sends*, *Curve*, *Timeline*.
6. **Jouer.** La colonne grise à l'extrême gauche de la liste porte le pointeur : la cue marquée ▶ est la cue en attente (standby), celle que GO lancera. Sa ligne est éclairée en ambre et son nom s'affiche à côté du bouton GO. Cliquez dans cette colonne sur n'importe quelle cue pour y placer le ▶, ou utilisez ↑ / ↓. Tant qu'aucune cue ne le porte, le haut de la liste affiche « no standby » et GO n'a rien à lancer. Survolez ou cliquez le ▶ en haut de la colonne pour un rappel. **Espace** fait GO.
7. **Enregistrer** avec Ctrl+S (⌘S sur Mac). La sauvegarde automatique tourne de toute façon.

### Les touches qui comptent pendant le spectacle

| Touche | Effet |
| --- | --- |
| Espace | GO — lancer la cue en attente |
| ↑ / ↓ | Déplacer l'attente sur la cue précédente / suivante |
| Échap | Tout arrêter proprement : fondu sur la durée de panique (1 s par défaut), fins de groupes jouées |
| Échap deux fois (en moins de 0,75 s) | Tout couper d'un coup, sans les fins |
| F9 | **Doh!** — reprendre le dernier GO |
| F | Marquer d'un drapeau les cues sélectionnées, pour y revenir après le filage (F à nouveau retire le drapeau) |
| Maj+F | Marquer d'un drapeau la cue lancée par le dernier GO |
| Ctrl+Z / Ctrl+Maj+Z | Annuler / rétablir une modification (⌘ sur Mac) |
| Ctrl+L | Verrouiller le spectacle pour éviter toute modification accidentelle |
| Ctrl+T | Charger à un instant : repartir du milieu d'une scène |

**Doh!** existe pour l'erreur qui arrive de temps en temps : un GO parti un temps trop tôt. Dans une courte fenêtre (réglable dans l'onglet *Playback*), F9 arrête ce que ce GO a lancé, remet l'attente en place et ramène les appareils dans l'état d'avant. Ce que ce GO a arrêté revient là où il en serait, en fondu : une cue, ou un groupe sampler avec les clips qu'il jouait sur ses faders et ses pads. Le son déjà sorti des enceintes ne se rattrape pas, et Go.dot ne prétend pas le contraire.

Deux GO à moins d'une demi-seconde d'intervalle sont pris pour un rebond du doigt : le second est refusé. Réglable dans l'onglet *Playback*.

## Ce que vous pouvez essayer aujourd'hui

Tout ce qui suit est construit. Les sections marquées nouveau sont arrivées après la v0.1 et demandent la version la plus récente. Chaque ligne dit où le trouver.

### Jouer du son

| Fonction | Ce qu'elle fait | Où |
| --- | --- | --- |
| Cues média | Jouer un fichier vers des sorties nommées à des niveaux donnés, avec pre-wait, post-wait et enchaînement | Glisser un fichier sur la liste |
| Points d'entrée et de sortie, boucles | Couper un fichier, boucler une zone, sortir de la boucle sur une cue *Advance* ; quand le dernier point de sortie s'arrête avant la fin du fichier, un bouton sous la liste des zones fait du reste du fichier une nouvelle zone | Panneau *Waveform* ; *transport > Advance* |
| Courbe de niveau | Dessiner une courbe de volume sur la forme d'onde, ou l'enregistrer depuis un fader | Panneau *Waveform* |
| Vitesse | De 0 à 20×, en varispeed (la hauteur suit) ou en timestretch (la hauteur reste), fondus de vitesse, et lecture à l'envers ou en aller-retour entre les points d'entrée et de sortie | Inspecteur, *what it does* |
| EQ | Quatre bandes et deux filtres sur chaque cue | Panneau *EQ* |
| Plugins | Une chaîne VST3, AU ou LV2 sur chaque cue. Les plugins tournent dans un processus à part : un plugin qui plante rend sa cue muette, pas le spectacle | Panneau *FX* ; *Show settings > Plugins* pour le scan |
| Envois | Niveaux d'une cue vers les canaux de mixage du spectacle | Panneau *Sends* |
| Courbes d'envoi | Dessiner le niveau de chaque envoi sur le fichier, comme la courbe de niveau le fait pour le volume | Panneau Waveform, choix de la courbe |
| Monter un son (nouveau) | Couper un son en sections à la tête de lecture, les réordonner ou en retirer, régler le fondu enchaîné à chaque jointure et un trim par section, puis *Freeze* pour rendre le montage dans un nouveau fichier que la cue joue ; *Unfreeze* ramène les sections. Les courbes et les ranges restent sur le son qu'elles couvrent | Panneau *Waveform*, ligne des sections |
| Poignées sur un son (nouveau) | Sur la forme d'onde chaque section a des poignées : le carré au milieu est son volume ; à chaque bout, la poignée du bas déplace le bord, celle du haut la longueur du fondu (Maj : ce côté seul). La molette sur un fondu en courbe la forme (Maj : ce côté seul). Glisser dans la moitié haute sélectionne du temps ; cliquer dans la moitié basse choisit une section, y glisser la fait coulisser dans le temps. **x** coupe à la tête de lecture ou aux bouts de la sélection, **Retour arrière** supprime en laissant du silence, **Maj+Retour arrière** referme le trou | Panneau *Waveform*, cliquer dedans d'abord |
| Clic droit sur la forme d'onde (nouveau) | Clic droit sur la tête de lecture, ou n'importe où et elle y vient, pour *Set to start*, *Set to end*, *Add marker*, *Add new slice* et *Split*. Double-cliquer entre deux repères - marqueurs, changements de plan, bords des tranches, coupes du montage, début et fin - sélectionne ce qui est entre eux ; Maj+clic sur un autre intervalle l'ajoute aussi. Clic droit sur la sélection pour *Set start and end*, *Make new slices…* (taper combien), *Trim* (coupe le montage aux deux bouts) et *Remove* (l'enlève et referme le trou). Un marqueur est un losange en haut : le glisser le déplace, clic droit ou double-clic pour le renommer ou le supprimer | Panneau *Waveform* |
| Modèles de cue | Garder sous un nom les réglages d'une cue média ou vidéo, et en faire naître de nouvelles cues | Menu media ▾ ; Edit > Save as template… ; Show settings > Templates |
| La molette sur un nombre (nouveau) | Tourner n'importe quel nombre de l'inspecteur à la molette de la souris, sans cliquer : un niveau le long de la course d'un fader, un temps d'un dixième de seconde par cran ; Maj pour des pas plus fins | Inspecteur |

### Organiser le temps

| Fonction | Ce qu'elle fait | Où |
| --- | --- | --- |
| Groupes | *Timeline* (les membres partent ensemble), *Sequential on GO*, *Sequential automatic*, *Shuffle* (nouvel ordre à chaque tour), *Sampler* (sons et images lancés depuis pads et faders, le fader d'une image réglant son opacité), *Time group* (ses membres partent quand le LTC, le MTC ou l'horloge atteint leur heure : voir *Timecode en entrée*) | *Add > group* |
| Enchaîner les membres d'une séquence (nouveau) | Choisir une fois pour tout le groupe comment un membre mène au suivant : *Let play* (chaque membre tel quel), *Gapless* (le suivant part exactement à la fin du précédent) ou *Crossfade*, avec une durée et une courbe. Dans un groupe automatique, le membre suivant est placé sur la fin du précédent ; dans un groupe joué au GO, chaque GO coupe ou fond ce qui joue encore dans le groupe. Les images se fondent l'une dans l'autre. La boucle d'un son peut fondre ses tours de la même façon | *Join* d'un groupe séquentiel dans l'inspecteur ; *range wraps* d'un son |
| Fondus | Faire varier niveau, envois (vers les aux aussi), EQ et paramètres de plugins de n'importe quelle cue, groupe ou aux, selon une courbe dessinée. Un fondu sur une cue vidéo fait varier son opacité, sa place, sa taille et sa rotation, la vitesse d'un film ou un flou. Chaque fondu a les panneaux de sa cible, avec ses changements affichés : *Mixer*, *EQ* et *FX* pour un son ou un aux, *Picture* pour une cue vidéo, où l'on fait glisser le cadre là où le fondu emmène l'image. Doh! remet en place ce que le fondu du GO a changé (nouveau) | *Add > fade*, puis *Mixer*, *EQ*, *FX*, *Picture*, *Curve*. Les mêmes boutons sont en haut de l'inspecteur et dans l'en-tête du panneau (nouveau) |
| Cues de transport | Arrêter, arrêter après ce membre ou ce tour, lancer une autre cue ; activer ou désactiver une cue pour ce passage ; déplacer l'attente sur une cue, ou y sauter et faire GO | *Add > transport* |
| Noms automatiques (nouveau) | Une cue que personne n'a nommée porte le nom de ce qu'elle fait : un fondu ou un arrêt celui de sa cible (*Fade out Intro music*), une cue OSC ou MIDI son message, un son, une image ou un micro son fichier ou son entrée. Il suit quand la cible, le fichier ou le message change. Un nom tapé reste ; effacé, le nom automatique revient | Colonne du nom ; inspecteur, *Name* |
| Déclencheurs | Lancer des cues sur OSC, MIDI entrant, à une heure donnée, ou sur une touche du clavier de l'ordinateur - toute touche sauf celles du spectacle (Espace, Échap, F9 et les raccourcis de la fenêtre). Le standby ne bouge pas quand un déclencheur part | Inspecteur, *when* ; la ligne *Key* d'une cue, puis la touche |
| Chariots, *carts* (nouveau) | Une liste que le GO ne rencontre jamais, pour des cues lancées dans le désordre : par leurs déclencheurs, une touche, ou l'onglet *Cart* de la télécommande Android. Tout type de cue - un son, un message OSC ou MIDI, un script, un fondu - et un groupe comme macro : un groupe *Timeline* lance tout à la fois, une séquence automatique l'une après l'autre avec ses attentes. Un groupe sampler dans un chariot est armé tout le spectacle, et ses pads portent tout type de cue | Le **+** à côté des onglets de listes, *New cart*. Les onglets au-dessus de la liste montrent chaque liste ; ▸ marque celle sur laquelle agit le GO, et un double-clic en renomme une |
| Charger à un instant | Sauter au milieu d'une scène avec les bonnes cues aux bons endroits | Ctrl+T. Sur la console web, la barre *aim* : choisir une cue, glisser jusqu'à l'instant et lire ce qui jouerait, puis appuyer sur *load to time* |
| Anticipation | La cue en attente est chargée et ses appareils préparés avant le GO | Automatique |

### Son live

| Fonction | Ce qu'elle fait | Où |
| --- | --- | --- |
| Cues micro | Ouvrir une entrée nommée à travers une tranche de rack avec ses propres plugins | *Add > mic* ; *Show settings > Inputs*, *Rack* |
| Auxiliaires (nouveau) | Un retour de réverbération ou de délai que les cues alimentent par leurs envois : ses propres plugins, son niveau, sa sortie et ses envois, gardés avec le spectacle ; un fondu peut les déplacer le temps de la soirée. Un clic sur le nom d'un auxiliaire dans le panneau des envois d'une cue ouvre l'auxiliaire lui-même | *Show settings > Rack* : + Mono aux, + Stereo aux ; le panneau *sends* d'une cue |
| Échantillonnage live | Enregistrer du son pendant le spectacle, puis la boucler, la superposer ou l'effacer | Panneau *Take* ; *transport > On a take* |
| Enregistreur live | Garder l'instant de chaque cue lancée pendant un filage, dans une prise | Ctrl+Maj+R |

### Parler au reste de l'installation

| Fonction | Ce qu'elle fait | Où |
| --- | --- | --- |
| Cues OSC | Envoyer à une console, un processeur ou un serveur vidéo, depuis la liste des adresses de l'appareil : plusieurs messages ou bundles dans une cue, et des courbes jouées sur l'horloge de la cue, enregistrées depuis les retours de l'appareil ou une SpaceMouse. Une cue de milliers de messages - tout un snapshot d'un processeur - part au rythme que l'appareil accepte, réglé dans sa case *Per tick* | *Add > osc* ; *Show settings > Network* |
| Cues écrites par WFS-DIY et S21_HiJack (nouveau) | Enregistrer un snapshot dans WFS-DIY ou S21_HiJack le dépose dans Go.dot en une seule cue après la cue sélectionnée dans la liste, ou à la fin de la liste si rien n'est sélectionné ; l'enregistrer de nouveau met à jour la même cue. Chacun se déclare comme appareil, et ses propres adresses et noms de snapshots deviennent des menus, un par partie de l'adresse, sous la *target* de la cue | WFS-DIY : onglet *Network*, protocole Go.dot, puis *Write to Go.dot* ; S21_HiJack : *Setup > Cueing system : Go.dot* |
| Menus d'adresse et de valeur (nouveau) | Sur un appareil qui se décrit lui-même, choisir l'adresse partie par partie et la valeur dans sa liste | Inspecteur, sous *target* |
| Vos propres presets (nouveau) | Gardez des presets à vous à côté de ceux livrés : un fichier posé dans votre dossier de presets est proposé sous *Yours* quand vous ouvrez *PRESET*, et celui qui porte le nom d'un preset livré est utilisé à sa place. Enregistrez en un clic comme preset tout appareil qui se décrit lui-même - trouvé sur le réseau, déclaré de lui-même, ou créé depuis un preset que vous voulez modifier - et recréez-le dans n'importe quel spectacle, sur cet ordinateur ou sur un autre une fois le fichier copié. | *Show settings > Network* : *Save as a preset…* dans la cellule *Kind* de l'appareil ; *Show presets folder* |
| Appareils trouvés sur le réseau (nouveau) | Go.dot liste les appareils du réseau qui se décrivent en OSCQuery - un autre Go.dot, Chataigne, ossia score, tout ce qui s'annonce - et en fait un appareil du spectacle en un choix, ses adresses proposées en menus. Un appareil qui ne s'annonce pas se donne par son adresse. Go.dot s'annonce de la même façon, et ces logiciels le trouvent aussi. | *Show settings > Network > FOUND* |
| Presets d'appareils (nouveau) | Créer un appareil depuis un preset pour votre console ou votre processeur : Yamaha, DiGiCo, Allen & Heath, Behringer et Midas, SSL Live, ETC Eos, grandMA, Holophonix, L-ISA, d&b Soundscape, Spat Revolution, ADM-OSC, MIDI Show Control. Chaque protocole que l'appareil parle est un preset à part, les menus de la cue le suivent, et le message part sur le fil propre à l'appareil : OSC en UDP ou en TCP, le RCP de Yamaha, une ligne telnet ou du MIDI. Les lignes de l'appareil disent où il est (la cellule Where : par datagramme, par connexion, sur un port série, un port MIDI ou un broker MQTT) et comment on l'entend en retour. | *Show settings > Network > PRESET* |
| MQTT (nouveau) | Un capteur, un contrôleur ou une centrale qui parle MQTT est un appareil du spectacle : ses topics sont des adresses sous le préfixe de l'appareil, si bien qu'un patch, un script ou une cue OSC l'entend et y publie comme avec n'importe quel appareil. Go.dot se connecte à n'importe quel broker, et fait tourner son propre broker pour un appareil qui n'a nulle part ailleurs où publier. | *Show settings > Network* (le menu de la cellule d'adresse ; l'interrupteur *MQTT broker* sous la table) |
| Retour arrière Doh! | Quand un GO est repris, envoyer à chaque appareil la commande qui l'annule | Inspecteur ; onglet *Network* |
| Cues MIDI | Program change, control change, notes, pitch bend, SysEx | *Add > midi* |
| Liaisons, *bindings* (nouveau) | Relier un fader, un bouton, une note ou un pitch bend MIDI, ou une adresse OSC, à n'importe quel nombre du spectacle - le niveau d'une cue, un envoi, un gain d'EQ, un paramètre d'appareil - mis à l'échelle de sa plage, pour tout le spectacle ou seulement pendant qu'une cue joue. *Learn* : bougez la commande, elle est liée. Faders et boutons donnent la valeur, les encodeurs sans fin la tournent pas à pas, pédales et joysticks la font aller d'autant plus vite qu'ils sont poussés loin ; un bouton sans moteur rattrape la valeur avant de la déplacer, et un fader motorisé reçoit la valeur en retour | Clic droit sur un nombre dans l'inspecteur, *Bind a control to it* ; *Show settings > Bindings* |
| Timecode en sortie (nouveau) | Du MTC sur un port MIDI, ou du LTC SMPTE sur une sortie audio, depuis une cue. Il démarre au label que vous lui donnez (01:00:00:00 si vous ne dites rien) et tourne jusqu'à ce qu'on l'arrête, ou pendant une durée. Placé dans un groupe avec une chanson, les deux démarrent sur le même échantillon ; une deuxième cue de timecode reprend la ligne ; Échap l'arrête sur une limite d'image ; Doh! le met en pause et le GO suivant le reprend là où il s'était arrêté ; on le fait sauter en le glissant dans le panneau des cues en cours. On choisit une fois pour le spectacle la cadence d'images, le port MIDI et la sortie, et on donne au LTC une sortie à lui : elle ne porte rien d'autre. | *Add > midi > MTC*, *Add > audio > LTC timecode* ; *Show settings > Timecode* |
| Timecode en entrée (nouveau) | Lancer des sons, des images, des groupes timeline et des séquences automatiques quand le timecode entrant atteint leur heure. Sélectionner les cues, choisir *Add > group > Time group*, dire ce qu'il suit (*follows* : *LTC in*, *MTC in* ou *the clock*, l'horloge de l'ordinateur) et donner à chaque membre l'heure à laquelle il démarre (*starts at*). Le GO arme le groupe, qui reste armé jusqu'à ce qu'on l'arrête, comme une banque de sampler ; dans la section Persistent il reste armé tout le spectacle, et la cue en attente ne bouge pas quand un membre part. Un membre lancé par du LTC démarre sur l'échantillon exact où le code atteint son heure. Chaque membre dit aussi comment il suit le code (*chase*) : *free run* joue à sa propre vitesse une fois lancé ; *jump* revient en place par un fondu enchaîné de 10 ms dès qu'il s'écarte de plus de 10 ms ; *catch up, varispeed* et *catch up, timestretch* jouent jusqu'à 1 % plus vite ou plus lentement jusqu'à se recaler, le premier en changeant la hauteur, le second en la gardant, et sautent plutôt quand l'écart dépasse un quart de seconde. Quand le code s'arrête, les membres continuent de jouer ; quand il saute, les membres qui suivent sont replacés là où il dit et ceux en *free run* sont laissés tels quels. Go.dot garde sa propre horloge et mesure le code par rapport à elle, si bien que le reste du spectacle garde son propre temps quoi que fasse le code. On choisit une fois pour le spectacle l'entrée LTC et le port MIDI par lesquels le code arrive, et avec quelle précision les membres suivent ; l'onglet montre ce qui arrive. | *Add > group > Time group* ; *Show settings > Timecode > Coming in* |
| Moniteur réseau | Voir chaque message entrant et sortant | *Show > Network monitor…* |
| Surfaces de contrôle | Faders, pads, potentiomètres et DCA, sur une surface Mackie Control ou un panneau virtuel à l'écran. Prise en charge complète de l'Asparion D700 : faders, pages EQ et plugins sur les rotatifs, molette maître, attente sur les flèches | *Show > Surfaces…* |
| Stream Deck (nouveau) | Tous les modèles, branchés sur cet ordinateur. Un Stream Deck à touches joue un chariot : ses cues sur les touches avec leurs numéros, noms et couleurs, allumées pendant qu'elles jouent, et une touche pour tourner la page quand le chariot dépasse le nombre de touches. Cochez *Show keys* pour avoir GO, Esc, Doh! et les flèches de l'attente au bout de la dernière rangée. Sur un Stream Deck+, les touches du haut choisissent *Send*, *EQ*, *FX* ou *Loop* et les quatre molettes règlent ceux de la cue choisie, avec ce que chacune règle écrit au-dessus ; les touches du bas passent à la cue précédente ou suivante. Les réglages montrent les touches du Stream Deck, branché ou non, et appuyer sur une touche à l'écran l'appuie. Quand l'application Stream Deck d'Elgato est ouverte, Go.dot propose de la fermer | *Show settings > Surfaces > Add surface > Stream Deck* |
| Console web | Le même spectacle dans un navigateur, sur n'importe quelle machine du réseau | `http://<cette machine>:5010/ui` |
| La console web sur une tablette (nouveau) | Au doigt, celle d'un iPad comprise, comme la télécommande Android : **PANIC** arrête tout, et pressé de nouveau dans les 0,75 s coupe tout, avec **Doh!** à côté ; glissez une cue qui joue loin d'un côté ou de l'autre pour la couper, touchez-la pour ses arrêts ; touchez un membre d'un groupe qui joue pour le sauter ce tour-ci, tenez-le pour le sauter jusqu'à la fin du groupe ; faites glisser une tête de lecture, plus finement à mesure que le doigt s'en éloigne au-dessus ou au-dessous ; tenez une cue pour *Fire now*, *Park the standby here* et *Flag* ; *pick several* pour choisir des cues sans clavier ; faites glisser un nombre de côté pour le déplacer depuis là où il est, plus finement loin de la case. Une souris fait tout cela de la même façon | `http://<cette machine>:5010/ui` sur la tablette |
| Ports série | Recevoir les lignes d'un Arduino dans une cue process ou une cue script, et lui en renvoyer | Show settings > Serial |

### Vidéo (nouveau)

| Fonction | Ce qu'elle fait | Où |
| --- | --- | --- |
| Cues vidéo | Montrer un film, une image fixe, un aplat de couleur, un masque adouci ou une entrée live sur un canevas, avec GO, Échap et Doh! comme pour le son | *Add > video*, puis choisir le canevas |
| Retard du son ou de l'image (nouveau) | Un vidéoprojecteur montre l'image un peu en retard, une longue chaîne audio joue le son un peu en retard. On indique à la cue lequel retenir et de combien : *Delay* sur *Sound* ou *Picture*, et *Delay by* en millisecondes. L'autre part avec la cue ; celui qui est retenu part d'autant plus tard et finit d'autant plus tard, et la cue se termine quand les deux ont fini. Une cue son peut retenir son son pour s'aligner sur une image lancée en même temps, et le son d'un film suit le film | Inspecteur, *Delay* et *Delay by* |
| Lecture HAP | HAP, HAP Alpha et HAP Q jouent à pleine vitesse. Les autres films jouent en aperçu et se convertissent en HAP en arrière-plan (FFmpeg est téléchargé à la première utilisation) | Automatique ; inspecteur |
| Cache d'analyse | Les couleurs, niveaux et bandes de film calculés par Go.dot sont gardés à côté des médias et réutilisés la fois suivante ; ceux des fichiers qui ne sont plus dans le dossier des médias sont nettoyés à chaque lancement | Automatique ; *Show > Clean up the analysis cache* |
| Bande du film | Vignettes le long de la bande, changements de plan marqués ; la tête de lecture et les points d'entrée et de sortie s'accrochent à un changement de plan (Alt les libère) ; les cases *Scene changes* et *Snap to scene changes* désactivent l'un ou l'autre ; le son lié au film s'édite à côté | Haut de l'inspecteur |
| Monter un film (nouveau) | Couper un film HAP en sections à la tête de lecture, sur sa grille d'images ; les réordonner ou en retirer ; régler le fondu à chaque jointure. Son son verrouillé est coupé en même temps et garde ses courbes sur le son. *Freeze* rend film et son dans de nouveaux fichiers que les cues jouent ; *Unfreeze* ramène les sections. Un film qui n'est pas en HAP le dit : le convertir d'abord | Panneau *Strip*, ligne des sections |
| Panneau image | Ajuster, remplir ou étirer ; échelle, déplacement, rotation, miroir ; contraste, saturation, gamma, teinte et quatre courbes ; fusion normale, addition, écran et produit | Panneau *Picture* |
| Incrustations (nouveau) | Rendre transparente une image, un film ou une caméra par sa propre couche alpha, par sa luminosité, par sa luminosité comme forme dans une couleur, ou par un fond bleu ou vert dont la couleur est retirée des bords | Panneau *Picture*, *Key* |
| Cues texte (nouveau) | Titres et diapositives : des mots dans une police, une taille et une couleur, avec un contour qui peut rayonner. Les mots peuvent contenir des champs comme `{score}`, que n'importe quel programme remplit en envoyant `/godot/text/score` en OSC - aussitôt, même quand le spectacle est verrouillé, et jamais enregistrés | *Add > video > Text* ; panneau *Picture*, *Fields* |
| Polices dans le spectacle (nouveau) | Un spectacle peut emporter ses propres polices, dans un dossier `fonts/` à côté de ses médias, déposées depuis le menu des polices : une police installée emportée, ou un fichier de police ajouté. Un titre a alors le même aspect sur toutes les machines, Mac ou PC, principale ou de secours, et *Save as* emporte les polices avec lui. La taille d'une police est celle des lettres, comme dans tout logiciel de mise en page | panneau *Picture*, menu des polices : *Carry it in the show*, *Add a font file...* |
| Cues flou (nouveau) | Floute tout ce qui est en dessous sur son canevas, dans ou hors d'un contour ; un fondu fait varier son intensité | *Add > video > Blur* ; panneau *Picture* |
| Canevas et mapping | Plusieurs canevas sur une sortie, chacun déformé sur le mur par un maillage, et un ASC CDL par sortie pour accorder les projecteurs. **Grid** affiche sur le projecteur une grille courbée par la déformation, pour régler la géométrie | *Show settings > Video* ; éditeur de déformation ; l'onglet *Mapping* de la télécommande Android |
| NDI, Spout, Syphon | Envoyer une sortie à un autre programme, recevoir l'image d'un programme comme entrée live, ou faire passer l'image d'une cue par un autre programme et retour | *Show settings > Video* |
| Cartes vidéo et caméras (nouveau) | Envoyer un canevas dans le câble SDI ou HDMI d'une carte Blackmagic DeckLink ou AJA, au format qu'attend le projecteur ou le mélangeur en aval, avec le son du spectacle dans le même câble ; sur une carte qui a un incrustateur, envoyer *fill* et *key* pour qu'un mélangeur vidéo pose vos images sur une autre. Recevoir une image live : d'une carte, ou d'une caméra - une webcam, un boîtier de capture USB, un ATEM Mini ou un Web Presenter en mode webcam - chacune nommée avec sa marque, au format choisi ou au meilleur qu'elle propose. Une carte demande le logiciel de sa marque (Desktop Video de Blackmagic, Desktop Software d'AJA), et `wfg video-probe` liste ce que savent faire vos cartes et vos caméras. Sur un Mac, la première caméra utilisée demande votre accord | *Show settings > Video*, menu *Display* d'une sortie, *Sent to a video card* ; *+ input*, type *Video capture*, puis l'appareil et son *Format* |
| Lecture anticipée | Les images fixes et films du prochain GO sont chargés avant l'appui, et chaque ligne de la liste dit si sa cue est prête | Liste de cues |
| DCA sur les images | Un DCA agit sur les images comme opacité ; le potentiomètre au-dessus d'une tranche DCA règle la courbe d'arrivée de l'image et le décalage du son | Surfaces ; panneau virtuel |
| Moniteur vidéo | La cue choisie seule à l'écran pendant qu'on la règle | S'ouvre avec la bande ou le panneau image |

Les images sont dessinées par un processus à part que Go.dot surveille : un film lent ne peut pas retenir une cue son.

### Cues process : Pure Data à l'intérieur (nouveau)

Une **cue process** contient un patch Pure Data et le fait tourner tant que la cue tourne, au rythme du contrôle, jamais dans le chemin audio. Elle entend ce qui arrive (retours des appareils, OSC, MIDI, lignes série) et envoie aux appareils, en MIDI et aux commandes de Go.dot. On édite le patch sur le canevas de Go.dot en pied de fenêtre, où les valeurs bougent en direct et où toggles, bangs, sliders et boîtes de nombre se jouent, ou on l'ouvre dans plugdata ou Pure Data : chaque sauvegarde revient. Des abstractions toutes faites couvrent les besognes courantes : `go.avg`, `go.minmax`, `go.smooth`, `go.scale`, `go.deadband`, `go.change`, `go.edge`, `go.hold` et `go.ratelimit`, chacune avec son patch d'aide, plus un spectacle d'exemple. *Add > process*.

Le **?** dans le coin du canevas ouvre un patch d'aide qui montre, avec des boîtes à copier, comment un patch entend et envoie :

| Pour... | Dans le patch |
|---|---|
| Entendre l'OSC d'un appareil (Show settings > Network, Rx activé) | `[r /son/adresse]` |
| Envoyer à un appareil, ici ou sur un autre ordinateur | `[s /son/adresse]` |
| Déclencher une cue | `[; /godot/cmd/cue/fire 3.1(` par son numéro, ou par son identifiant |
| Lancer une commande de Go.dot | `[s /godot/cmd/go]`, `standby/set`, `run/stopAll`... |
| Entendre ou régler une ligne du spectacle | `[r /godot/...]`, `[s /godot/...]` |
| Le MIDI (MIDI in et MIDI out de la cue) | `[notein]`, `[ctlin]`, `[midiin]`... et `[noteout]`, `[ctlout]`... |
| Un Arduino (Show settings > Serial) | `[r /godot/serial/<id>/in]`, `[s /godot/serial/<id>/out]` |
| Un capteur sur un broker MQTT (Show settings > Network, un appareil MQTT dont le préfixe est `/house`) | `[r /house/room1/temp]`, `[s /house/lights/1]` |
| La SpaceMouse | `[r /godot/puck]` |

Un clic droit sur une cue de la liste, ou sur un port de l'onglet Serial, puis *Copy for a patch* donne sa boîte avec son identifiant ; collez-la sur le canevas, ou dans une boîte de Pd. Une cue glissée de la liste sur le canevas devient une boîte qui la déclenche. Tous les patchs ouverts dans Pure Data partagent un seul Pd, sa fenêtre passe au premier plan, et sur un écran haute résolution les menus et les patchs de Pd sont dessinés à une taille lisible.

Un appareil qui parle MQTT s'ajoute dans l'onglet Network comme les autres : cliquez sa cellule d'adresse et choisissez *On an MQTT broker, at an address...* (le Mosquitto de la salle, un Home Assistant) ou *On Go.dot's MQTT broker*, que Go.dot fait tourner lui-même quand l'interrupteur *MQTT broker* sous la table est activé. Le même menu prend les topics qu'il écoute (`#` pour tout). Chaque topic est alors une adresse sous le préfixe de l'appareil : `room1/temp` sur un appareil dont le préfixe est `/house` est `/house/room1/temp`, et le contenu est lu comme une ligne série, les nombres en nombres.

### Cues script : JavaScript et Lua (nouveau)

Une **cue script** fait ce que fait une cue process, écrite en texte au lieu d'être dessinée : quelques lignes de JavaScript ou de Lua qui tournent tant que la cue tourne, au rythme du contrôle, jamais dans le chemin audio. Un script entend ce qui arrive (retours des appareils, OSC, MIDI, lignes série, lignes du spectacle) et envoie aux appareils, en MIDI et aux commandes de Go.dot. On choisit le langage dans l'inspecteur ; on écrit le script dans l'éditeur en pied de fenêtre, où une erreur est marquée sur sa ligne et où ce que le script affiche apparaît sous le code. *Open in your editor* le confie à l'éditeur de texte de votre ordinateur, et chaque sauvegarde revient. *Add > script*.

```js
// Double ce que la console renvoie pour le x de la source 1, et déclenche la cue 3.1 au-delà de 0.8.
godot.hear('/wfs/source/1/x', (x) => {
    godot.send('/wfs/source/2/x', x * 2);
    if (x > 0.8) godot.fire('3.1');
});
```

Le **?** à côté de l'éditeur ouvre une fiche d'aide dans le langage du script, avec une ligne à copier pour chaque entrée et chaque sortie, et des scripts tout faits pour démarrer (une moyenne, un minimum et un maximum, un lissage, un seuil franchi) :

| Pour... | En JavaScript | En Lua |
|---|---|---|
| Entendre l'OSC d'un appareil (Show settings > Network, Rx activé) | `godot.hear('/son/adresse', (x) => ...)` | `godot.hear('/son/adresse', function (x) ... end)` |
| Envoyer à un appareil | `godot.send('/son/adresse', 0.5)` | pareil |
| Déclencher une cue, par son numéro ou son identifiant | `godot.fire('3.1')` | pareil |
| Lancer une commande de Go.dot | `godot.command('go')`, `standby/set`, `run/stopAll`... | pareil |
| Entendre ou régler une ligne du spectacle | `godot.hear('/godot/...', ...)`, `godot.set('cue/<id>/name', 'Pluie')` | pareil |
| Le MIDI (MIDI in et MIDI out de la cue) | `godot.midi.hear(...)`, `godot.midi.send(0x90, 60, 100)` | pareil |
| Un Arduino (Show settings > Serial) | `godot.hear('/godot/serial/<id>/in', ...)`, `godot.send('/godot/serial/<id>/out', ...)` | pareil |
| Un capteur sur un broker MQTT (Show settings > Network, un appareil MQTT dont le préfixe est `/house`) | `godot.hear('/house/room1/temp', ...)`, `godot.send('/house/lights/1', 1)` | pareil |
| La SpaceMouse | `godot.hear('/godot/puck', ...)` | pareil |
| Plus tard, de temps en temps, à chaque tick | `godot.after(1.5, ...)`, `godot.every(0.25, ...)`, `godot.tick(...)` | pareil |

Un clic droit sur une cue de la liste, ou sur un port de l'onglet Serial, puis *Copy for a script* ou *Copy for a Lua script* donne sa ligne avec son identifiant ; une cue glissée de la liste sur l'éditeur y tape la ligne qui la déclenche. Un script n'a ni fichiers, ni réseau, ni modules à lui : il passe par Go.dot pour atteindre le monde, si bien que le journal du spectacle garde ce qu'il a fait. Un script pris dans une boucle sans fin est arrêté au bout d'une seconde, et le spectacle continue. Le spectacle d'exemple *Process examples* contient le même capteur de lumière qu'en patch, en JavaScript et en Lua. Un appareil MQTT s'entend et se publie par ses adresses, comme dans le tableau ; le script tout fait *relay* entend un capteur sur un broker, déclenche une cue au passage d'un seuil et publie l'état d'une lumière.

### Importer des spectacles QLab et Ableton Live (nouveau)

- **QLab** : File > Import QLab workspace… lit directement le fichier d'un espace de travail QLab 4 ou 5, sans QLab. Cochez les listes de cues voulues ; Go.dot écrit un nouveau spectacle (cues, groupes, niveaux, fondus, routage et cibles) et un rapport de ce qu'il n'a pas pu reprendre. Les cues texte et vidéo suivent aussi (nouveau) : chaque stage QLab devient un canvas de sa taille, ses sorties attendant qu'on leur choisisse un projecteur, et films, images fixes et titres sont placés et fondus comme dans QLab. Certains nombres propres à QLab sont encore vérifiés sur un espace de travail de test, et le rapport nomme les polices d'un titre que cette machine n'a pas.
- **Ableton Live** : l'import décompresse le fichier .als (un document XML gzippé) et lit le XML qu'il contient pour traduire le set Live en cues et groupes, pour faire passer à la liste de cues un spectacle construit en session. Une scène devient un GO, et les clips muets « track in » deviennent les fondus qu'ils représentent. L'import lit Live 10 à 12, fait d'une tournée un seul spectacle avec une représentation par lieu, et se trouve dans File > Import Ableton Live set… Un retour qui porte des effets arrive désormais en auxiliaire, ses envois entendus : les effets propres à Live (Reverb, Delay…) ne se chargent pas hors de Live, l'auxiliaire est donc laissé vide pour un plugin de votre choix, et le rapport dit lesquels s'y trouvaient.

### Protéger le spectacle

- **Historique d'annulation** de chaque modification (Ctrl+Maj+U), et sauvegarde automatique continue.
- **Verrouiller** le spectacle pendant la représentation (Ctrl+L) : GO, Échap et Doh! fonctionnent, l'édition non.
- **Ce qui manque à cet ordinateur, montré (nouveau).** Une cue qui a besoin de quelque chose d'absent ce soir - l'interface audio, un écran, un appareil MIDI, un plugin, un fichier - porte un avertissement et un mot après son nom (*no audio*, *no screen*...) ; survolez-le pour savoir pourquoi. Un bouton sur la ligne de titre ouvre l'onglet de *Show settings* concerné.
- **Drapeaux (nouveau).** Quelque chose à corriger ? F sur la cue sélectionnée, ou Maj+F pour la cue lancée par le dernier GO, même spectacle verrouillé. Un drapeau se place juste à gauche du type de la cue jusqu'à ce que F le retire. Les drapeaux sont gardés à l'enregistrement du spectacle.
- **Spectacle et représentations.** Le spectacle est l'œuvre ; chaque soir est une représentation, une copie que l'on modifie sans toucher l'original. *File > New performance…*. À la fin, Go.dot liste ce qui a changé, cue par cue, pour reporter les bonnes modifications dans le spectacle.
- **Problèmes d'interface.** Si l'interface audio disparaît, le spectacle se met en pause et reprend à son retour. Si sa fréquence d'échantillonnage change, Go.dot suit.
- **Double-clic sur un `.wfg`** pour ouvrir le spectacle (installateur Windows, .deb Linux, macOS).

### Deux ordinateurs : un principal et un secours (nouveau)

Deux ordinateurs peuvent jouer ensemble un même spectacle. Le **principal** (*Main*) le joue et parle à l'installation ; le **secours** (*Backup*) le suit avec un instant de retard, démarre chaque son sur le même échantillon, et prend la main si le principal s'arrête.

**Ce qu'il faut à chaque ordinateur.** Sa propre copie du dossier du spectacle, sons et films compris : rien n'est copié de l'un à l'autre par le réseau. Sa propre interface audio, et ses propres sorties vidéo si le spectacle a des images. Le son et les images basculent sur votre console ou votre matrice : Go.dot ne les commute pas. Les deux sur un même réseau, ou sur deux pour un chemin de secours.

**Le réglage.** Sur chaque ordinateur, *Show settings > Redundancy* : dites si cet ordinateur est le *Main* ou le *Backup*, donnez l'adresse de l'autre (son adresse IP ; le port 5010 est sous-entendu ; deux adresses séparées par une espace pour deux réseaux), et la durée de silence au bout de laquelle le lien est perdu (1 s par défaut). Ces réglages appartiennent à l'ordinateur, pas au spectacle : copier le spectacle sur l'autre ordinateur ne les emporte pas.

**Ce qu'il dit.** À côté du nom du spectacle, en haut de la fenêtre, chaque ordinateur dit ce qu'il est : *Main - sending* (il envoie), *Main - alone* (aucun secours avec lui), *Backup - following* (il suit), *Backup - waiting* (il n'a pas trouvé son principal depuis son démarrage), *Backup - sending* (il a pris la main), *Main - following* (un principal redémarré après que son secours a pris la main). Quand il faut intervenir, un bandeau sous cette ligne le dit, avec le bouton qui y répond :

| Le bandeau | Ce qu'il veut dire | Le bouton |
| --- | --- | --- |
| *Backup - waiting* | Le secours n'a pas trouvé son principal ; il n'envoie rien | **Take over** : il envoie désormais |
| *Two mains!* | Les deux ordinateurs envoient, en général après le retour du lien suivant une prise de main : chaque commande arrive deux fois aux appareils | **Make this the main**, sur l'ordinateur qui doit garder le spectacle ; l'autre le suit |
| *Following, but not in step* | Quelque chose diffère entre les deux au même instant, par exemple une cue qui ne tourne que sur l'un | **Realign** : le secours reprend la position du principal |
| *Following, but the show differs* | Les deux spectacles ne sont pas les mêmes, par exemple une modification faite pendant que le secours était éteint | **Take the main's show** : le secours prend les cues et les réglages du principal, garde sa propre interface et ses ports, et met ses anciens fichiers de côté dans un dossier daté du spectacle. Les sons ne sont jamais copiés : un fichier absent d'un ordinateur est nommé, et c'est à vous de le copier |

Le principal en est informé aussi : quand le secours n'est pas en phase ou que son spectacle diffère, le bandeau du principal le dit avec les mots du secours, et son onglet *Redundancy* montre si le secours est en phase et a le même spectacle - les boutons qui y remédient sont sur le secours.

Les mêmes commandes, avec **Hand over** (le principal laisse le secours envoyer), se trouvent dans le menu *Show > Redundancy*, qui fonctionne spectacle verrouillé, sur l'onglet *Redundancy* avec ce que fait chacune, et sur la ligne sous le transport de la console web.

**Quand le principal s'arrête.** Le secours ne l'entend plus et, après le délai du lien, prend la main : il envoie désormais aux appareils et dit *The main was lost and this machine took over*. Un Échap ou un double Échap pressé sur le secours à ce moment-là est appliqué ; tout le reste que le principal n'avait pas confirmé est abandonné. Basculez votre console ou votre matrice sur le secours. Quand le principal redémarre, il trouve le secours en train d'envoyer et le suit.

**Le temps.** Tant qu'il suit, le secours n'envoie rien aux appareils (OSC, MIDI, série) ; une commande que vous lui donnez est transmise au principal. Les deux démarrent chaque son sur le même échantillon : sur deux ordinateurs en Dante, à environ un dixième de milliseconde. L'onglet *Redundancy* montre à quel point ils restent ensemble, et la latence de sortie de chaque interface.

### La télécommande Android (nouveau)

Go.dot a une application Android pour tablette ou téléphone. Elle reste reliée en arrière-plan : elle est à jour dès qu'on la regarde, et l'on passe à une autre application et retour sans attendre.

**L'obtenir.** Installez l'APK de la dernière version publiée sur [github.com/pob31/go.dot-remote/releases](https://github.com/pob31/go.dot-remote/releases) (Android 8 ou plus récent ; laissez le navigateur l'installer quand Android le demande). Dans l'onglet *Connexions*, ajoutez l'ordinateur qui fait tourner Go.dot : son adresse IP et le port 5010, l'adresse de la console web. La tablette et l'ordinateur doivent être sur le même réseau. Elle est en français quand la tablette l'est, et les noms ci-dessous sont ceux qu'elle affiche alors.

**Toujours à l'écran.** **GO**, **Doh!** et **PANIC** (deux fois en moins de 0,75 s coupe tout), la cue en cours et la suivante avec ses notes, des flèches pour déplacer la cue en attente, et, en toutes lettres, un spectacle verrouillé ou une commande refusée. Au bout de la barre d'onglets, une marque par connexion : un point vert quand elle est vivante, un anneau orange pendant qu'elle se relie, une croix rouge quand elle est perdue. Un appui ouvre *Connexions*.

| Onglet | Ce qu'on y fait |
| --- | --- |
| *Cues* | Toutes les listes. Touchez une cue pour la régler : niveau, trim pendant qu'elle joue, envois, les 16 réglages d'EQ, son DCA, attentes, vitesse, active ou non, drapeau et notes, avec sa forme d'onde. Touchez le bord gauche pour y placer la cue en attente ; appui long pour *Lancer maintenant* |
| *En cours* | Tout ce qui joue : arrêter, couper (un long balayage ou le menu), avancer, sauter un membre d'un tour, déplacer la tête de lecture, et un grand fader de trim pour chaque son |
| *Surface* | Les surfaces du spectacle avec leurs tranches : une D700 débranchée fonctionne depuis la tablette. *Cette tablette* donne un fader par DCA. Des pages *EQ* et *Envois* pour le son sélectionné |
| *Cart* | Les cues de chaque chariot en pads : touchez-en un pour la lancer, aussi fort que l'endroit où le doigt se pose ; une cue qui joue tant qu'on la tient s'arrête quand on lâche. Présent tant que le spectacle a un chariot |
| *Mapping* | La déformation et les zones de chaque sortie vidéo : glisser un point ou le pousser pas à pas, changer la grille de points, l'opacité et le mode de fusion d'une zone, **Identifier** pour reconnaître le projecteur, et **Grille**, qui affiche sur le projecteur une grille courbée par la déformation |
| *Script* | La page de texte de Choufleur, une fois son adresse donnée (plus bas) |
| *Connexions* | Les ordinateurs, un principal et son secours ensemble avec leurs boutons *Prendre la main* ; le nom de la tablette ; retrouver la tablette ; Choufleur et son vibreur de poignet ; *Quitter la télécommande* |

**Régler les niveaux d'un spectacle verrouillé.** Le niveau d'une cue fait partie du spectacle, le verrou le protège. On règle le trim du son qui joue, ou un DCA : l'un comme l'autre valent pour ce soir et ne sont jamais enregistrés. L'EQ et les envois se règlent en direct, comme sur la D700.

**Retrouver la tablette.** Dans Go.dot, *Show settings > Network* liste les tablettes reliées avec un bouton **Find** : la tablette fait clignoter son écran et la lumière de son appareil photo, sonne son alarme et vibre jusqu'à ce qu'on la touche.

**Choufleur sur la tablette.** Dans *Connexions*, tapez l'adresse d'opérateur que donne Choufleur, ou scannez son code. L'onglet *Script* montre alors le texte qui suit la représentation, sous le GO. Un vibreur de poignet Choufleur se relie dans le même onglet et vous touche le poignet avant vos cues.

**L'arrêter.** *Connexions > Quitter la télécommande* coupe toutes les liaisons et l'arrête en arrière-plan. Au lancement suivant, elle se relie aux mêmes ordinateurs.

## Ce qui vient très bientôt

Ces fonctions sont conçues dans la spécification mais pas encore construites. L'ordre peut changer selon ce que demandent les testeurs : raison de plus pour envoyer vos retours.

### Intégration logicielle

| Système | Ce que Go.dot en fera |
| --- | --- |
| **WFS-DIY** (synthèse de front d'onde) | Piloter ses sources, positions et LFO, réservés par cue pour que deux cues ne se disputent jamais une entrée. Écrire ses snapshots dans Go.dot sous forme de cues fonctionne déjà (plus haut) |
| **XOA / Tight-WFS** | La même chose, pour le rendu ambisonique et WFS |
| **S21-HiJack** | Le piloter comme compagnon de la DiGiCo S21, sous sa propre branche d'adresses. Écrire ses snapshots dans Go.dot et être piloté par le GO de Go.dot fonctionnent déjà (plus haut) |
| **Millumin** | La porte de sortie pour les spectacles dont la vidéo dépasse Go.dot |

WFS-DIY, XOA/Tight-WFS et S21-HiJack viennent du même auteur que Go.dot, comme Choufleur, le suivi de texte que la télécommande Android montre déjà (plus haut). Ils restent des applications séparées, éventuellement sur d'autres machines ; Go.dot les dirige par le réseau.

### Intégration matérielle

- **Consoles son** : rappel de snapshots, mouvements de faders et de tranches, retour arrière Doh!, sous forme de presets (plus haut) : DiGiCo, Yamaha, Allen & Heath, Behringer et Midas, SSL Live ; toute console qui parle OSC ou MIDI fonctionne déjà comme appareil opaque.
- **Consoles lumière** : GO et rappel de cues par OSC, MIDI et MIDI Show Control - presets ETC Eos et grandMA (plus haut) - et sACN comme source entrante.
- **Surfaces de contrôle** : HUI, Icon, Behringer et PreSonus, après Mackie Control, la D700 et le Stream Deck (déjà là, plus haut). Une SpaceMouse fait déjà bouger les courbes des cues OSC.
- **Modèles d'appareils** : une bibliothèque partagée des presets que font les utilisateurs, pour les consoles et processeurs qui ne se décrivent pas eux-mêmes. Les presets livrés en sont les premières entrées, et les vôtres peuvent s'y ajouter (plus haut).

## Faire un retour

Cette version existe pour que d'autres que l'auteur disent ce qu'ils y ont trouvé. Dites-nous ce qui vous a plu, ce que vous changeriez et ce qui n'a pas marché : les trois orientent la suite. Chaque message aide, y compris « je n'ai pas compris comment… ».

**Où :** [github.com/pob31/go.dot/issues](https://github.com/pob31/go.dot/issues). Un compte GitHub gratuit suffit. Écrivez en français ou en anglais.

**Un rapport de bug est le plus utile avec :**

1. Le nom de la version (le nom du fichier téléchargé, *Help > About Go.dot*, ou la première ligne de `wfg --version`).
2. Votre système et votre interface audio.
3. Ce que vous avez fait, ce que vous attendiez, ce qui s'est passé.
4. Le journal le plus récent (voir le tableau dans *Installer et lancer*).
5. Si possible, le dossier du spectacle, zippé. Laissez de côté les médias que vous ne pouvez pas partager.

**Une suggestion est la plus utile avec** la situation d'où elle vient : le spectacle, le moment précis, et ce que vous faites aujourd'hui dans QLab, Live ou sur la console pour contourner le problème. « Pendant les saluts, j'ai besoin de… » mène à une meilleure fonction que « merci d'ajouter X ».

**Ce qui est particulièrement recherché en ce moment :**

- Les premières impressions sur la fenêtre, de la part de gens qui ne l'ont jamais vue.
- Des interfaces, pilotes et systèmes que nous n'avons pas essayés.
- Les surfaces de contrôle et consoles que vous aimeriez voir dialoguer avec Go.dot, avec un lien vers leur documentation OSC ou MIDI.
- Des espaces de travail QLab et des sets Live que vous accepteriez de partager pour tester les imports.
- Votre console, votre processeur ou votre source de timecode avec Go.dot : les presets d'appareils et le timecode en entrée et en sortie ont été construits d'après les documents publiés, alors dites-nous ce que le vôtre a fait.

**Les contributions de code** sont bienvenues aussi. Les instructions de compilation et les conventions du dépôt sont dans le README, sections *Building* et *Contributing*. Go.dot est sous GPL-3 : ce que vous apportez reste libre.
