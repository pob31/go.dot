# Go.dot — Pour l'essayer

Oct 9, 2026 · @Pierre-Olivier Boulant

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

Ceci est une **version de test** (v0.1), faite pour essayer en salle de répétition ou chez soi, pas encore pour une première.

L'interface du logiciel est en anglais ; les noms des menus et des boutons sont donc cités en anglais ci-dessous.

## Installer et lancer

Téléchargez la version de test pour votre système sur la [page des versions](https://github.com/pob31/go.dot/releases), installez-la et lancez Go.dot. Il ouvre un spectacle vide, *Untitled*, sur votre interface audio par défaut, avec les réglages du spectacle ouverts pour en choisir une autre.

| Système | Téléchargement | Premier lancement | Journal pour les rapports de bug |
| --- | --- | --- | --- |
| Windows 10/11 | `-setup.exe` (installateur : menu Démarrer, ouverture des `.wfg` par double-clic) ou `.zip` (à décompresser où vous voulez, lancer `Go.dot.exe`) | Pas encore signé : SmartScreen avertit, choisir *Informations complémentaires* puis *Exécuter quand même* | `%APPDATA%\Go.dot\logs` |
| macOS 13.3+ (Apple silicon et Intel) | `.dmg`, glisser Go.dot dans Applications | Signé et notarié, s'ouvre sans avertissement. Autoriser l'accès au micro pour les cues micro | `~/Library/Logs/Go.dot` |
| Linux (compilé sur Ubuntu 24.04) | `.deb` (`sudo apt install ./go.dot-…deb`) ou archive tar (lancer `./go.dot.sh`) | Demande ALSA, FreeType, fontconfig, X11 (présents sur tout bureau) | `~/.local/state/Go.dot/logs` |

**Sous Linux avec une interface multicanal**, PipeWire ne donne que deux canaux via ALSA. Installez `pipewire-jack`, passez le profil de l'interface en *Pro Audio* dans pavucontrol, et choisissez JACK dans les réglages du spectacle.

**Sous Windows**, les pilotes ASIO sont pris en charge et sont le meilleur choix pour une interface pro.

Quelques vérifications depuis un terminal (sous Windows, `wfg.exe` dans le dossier d'installation ; sous macOS, dans `Go.dot.app/Contents/MacOS/`) :

- `wfg --version` — quelle version vous avez
- `wfg devices` — les interfaces audio utilisables
- `wfg midi` — les ports MIDI visibles
- `wfg plugins --scan` — trouver vos plugins VST3, AU et LV2
- `wfg validate <spectacle>` — vérifier un dossier de spectacle et lister chaque problème

Le dernier onglet de *Show settings*, **Getting started**, reprend chaque onglet de réglages dans l'ordre où l'on monte un spectacle, avec une phrase sur son rôle.

## Votre premier spectacle en dix minutes

Un spectacle est un dossier avec au moins un fichier `.wfg` et un dossier `media/` où Go.dot copie vos sons. Rangez-le où vous voulez, emportez-le sur une autre machine : seuls les réglages audio sont à revoir.

1. **Choisir l'interface audio.** *Show > Show settings… > Audio*. Le bouton GO passe au jaune quand l'audio tourne.
2. **Créer et nommer les sorties** (canaux de mixage ou sorties directes, mono ou stéréo). Dans *Outputs*, créez les sorties du spectacle (« Face G/D », « Sub », « Retours »). Dans *Output patch*, indiquez sur quel canal physique chacune arrive. Les cues visent des noms : changer de salle, c'est changer de patch, sans toucher aux cues.
3. **Ajouter des sons.** Glissez des fichiers WAV, AIFF, FLAC ou Ogg sur la liste. Chacun devient une cue média au nom du fichier. Déposez-en un sur une cue existante pour remplacer son fichier.
4. **Ajouter d'autres cues** depuis la rangée *Add* au-dessus de la liste : `memo`, `media`, `mic`, `video`, `fade`, `transport`, `osc`, `midi`, `process`, `group`. Ceux qui portent un ▾ ouvrent un petit menu de variantes.
5. **Régler chaque cue** dans l'inspecteur à droite : nom, pre-wait, post-wait, enchaînement, niveau. Les boutons en haut de l'inspecteur ouvrent les panneaux en pied de fenêtre : *Waveform*, *EQ*, *FX*, *Sends*, *Curve*, *Timeline*.
6. **Jouer.** Cliquez à l'extrême gauche d'une cue pour y déplacer le pointeur de lecture. La cue en attente (standby) est signalée par une marque jaune. **Espace** fait GO.
7. **Enregistrer** avec Ctrl+S (⌘S sur Mac). La sauvegarde automatique tourne de toute façon.

### Les touches qui comptent pendant le spectacle

| Touche | Effet |
| --- | --- |
| Espace | GO — lancer la cue en attente |
| ↑ / ↓ | Déplacer l'attente sur la cue précédente / suivante |
| Échap | Tout arrêter proprement : fondu sur la durée de panique (1 s par défaut), fins de groupes jouées |
| Échap deux fois (en moins de 0,75 s) | Tout couper d'un coup, sans les fins |
| F9 | **Doh!** — reprendre le dernier GO |
| Ctrl+Z / Ctrl+Maj+Z | Annuler / rétablir une modification (⌘ sur Mac) |
| Ctrl+L | Verrouiller le spectacle pour éviter toute modification accidentelle |
| Ctrl+T | Charger à un instant : repartir du milieu d'une scène |

**Doh!** existe pour l'erreur qui arrive de temps en temps : un GO parti un temps trop tôt. Dans une courte fenêtre (réglable dans l'onglet *Playback*), F9 arrête ce que ce GO a lancé, remet l'attente en place et ramène les appareils dans l'état d'avant. Le son déjà sorti des enceintes ne se rattrape pas, et Go.dot ne prétend pas le contraire.

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
| Poignées sur un son (nouveau) | Sur la forme d'onde chaque section a des poignées : le carré au milieu est son volume ; à chaque bout, la poignée du bas déplace le bord, celle du haut la longueur du fondu (Maj : ce côté seul). La molette sur un fondu en courbe la forme (Maj : ce côté seul). Glisser dans la moitié haute sélectionne du temps, cliquer dans la moitié basse choisit une section. **x** coupe à la tête de lecture ou aux bouts de la sélection, **Retour arrière** supprime en laissant du silence, **Maj+Retour arrière** referme le trou | Panneau *Waveform*, cliquer dedans d'abord |
| Modèles de cue | Garder sous un nom les réglages d'une cue média ou vidéo, et en faire naître de nouvelles cues | Menu media ▾ ; Edit > Save as template… ; Show settings > Templates |

### Organiser le temps

| Fonction | Ce qu'elle fait | Où |
| --- | --- | --- |
| Groupes | *Timeline* (les membres partent ensemble), *Sequential on GO*, *Sequential automatic*, *Shuffle* (nouvel ordre à chaque tour), *Sampler* (sons et images lancés depuis pads et faders, le fader d'une image réglant son opacité) | *Add > group* |
| Fondus | Faire varier niveau, envois, EQ et paramètres de plugins de n'importe quelle cue ou groupe, selon une courbe dessinée | *Add > fade*, puis *Mixer* et *Curve* |
| Cues de transport | Arrêter, arrêter après ce membre ou ce tour, lancer une autre cue ; activer ou désactiver une cue pour ce passage ; déplacer l'attente sur une cue, ou y sauter et faire GO | *Add > transport* |
| Noms automatiques (nouveau) | Une cue que personne n'a nommée porte le nom de ce qu'elle fait : un fondu ou un arrêt celui de sa cible (*Fade out Intro music*), une cue OSC ou MIDI son message, un son, une image ou un micro son fichier ou son entrée. Il suit quand la cible, le fichier ou le message change. Un nom tapé reste ; effacé, le nom automatique revient | Colonne du nom ; inspecteur, *Name* |
| Déclencheurs | Lancer des cues sur OSC, MIDI entrant ou à une heure donnée | Inspecteur, *when* |
| Charger à un instant | Sauter au milieu d'une scène avec les bonnes cues aux bons endroits | Ctrl+T |
| Anticipation | La cue en attente est chargée et ses appareils préparés avant le GO | Automatique |

### Son live

| Fonction | Ce qu'elle fait | Où |
| --- | --- | --- |
| Cues micro | Ouvrir une entrée nommée à travers une tranche de rack avec ses propres plugins | *Add > mic* ; *Show settings > Inputs*, *Rack* |
| Échantillonnage live | Enregistrer du son pendant le spectacle, puis la boucler, la superposer ou l'effacer | Panneau *Take* ; *transport > On a take* |
| Enregistreur live | Garder l'instant de chaque cue lancée pendant un filage, dans une prise | Ctrl+Maj+R |

### Parler au reste de l'installation

| Fonction | Ce qu'elle fait | Où |
| --- | --- | --- |
| Cues OSC | Envoyer à une console, un processeur ou un serveur vidéo, depuis la liste des adresses de l'appareil : plusieurs messages ou bundles dans une cue, et des courbes jouées sur l'horloge de la cue, enregistrées depuis les retours de l'appareil ou une SpaceMouse | *Add > osc* ; *Show settings > Network* |
| Retour arrière Doh! | Quand un GO est repris, envoyer à chaque appareil la commande qui l'annule | Inspecteur ; onglet *Network* |
| Cues MIDI | Program change, control change, notes, pitch bend, SysEx | *Add > midi* |
| Moniteur réseau | Voir chaque message entrant et sortant | *Show > Network monitor…* |
| Surfaces de contrôle | Faders, pads, potentiomètres et DCA. Prise en charge complète de l'Asparion D700 : faders, pages EQ et plugins sur les rotatifs, molette maître, attente sur les flèches | *Show > Surfaces…* |
| Console web | Le même spectacle dans un navigateur, sur n'importe quelle machine du réseau | `http://<cette machine>:5010/ui` |
| Ports série | Recevoir les lignes d'un Arduino dans une cue process, et lui en renvoyer | Show settings > Serial |

### Vidéo (nouveau)

| Fonction | Ce qu'elle fait | Où |
| --- | --- | --- |
| Cues vidéo | Montrer un film, une image fixe, un aplat de couleur, un masque adouci ou une entrée live sur un canevas, avec GO, Échap et Doh! comme pour le son | *Add > video*, puis choisir le canevas |
| Lecture HAP | HAP, HAP Alpha et HAP Q jouent à pleine vitesse. Les autres films jouent en aperçu et se convertissent en HAP en arrière-plan (FFmpeg est téléchargé à la première utilisation) | Automatique ; inspecteur |
| Cache d'analyse | Les couleurs, niveaux et bandes de film calculés par Go.dot sont gardés à côté des médias et réutilisés la fois suivante ; ceux des fichiers qui ne sont plus dans le dossier des médias sont nettoyés à chaque lancement | Automatique ; *Show > Clean up the analysis cache* |
| Bande du film | Vignettes le long de la bande, changements de plan marqués ; la tête de lecture et les points d'entrée et de sortie s'accrochent à un changement de plan (Alt les libère) ; les cases *Scene changes* et *Snap to scene changes* désactivent l'un ou l'autre ; le son lié au film s'édite à côté | Haut de l'inspecteur |
| Monter un film (nouveau) | Couper un film HAP en sections à la tête de lecture, sur sa grille d'images ; les réordonner ou en retirer ; régler le fondu à chaque jointure. Son son verrouillé est coupé en même temps et garde ses courbes sur le son. *Freeze* rend film et son dans de nouveaux fichiers que les cues jouent ; *Unfreeze* ramène les sections. Un film qui n'est pas en HAP le dit : le convertir d'abord | Panneau *Strip*, ligne des sections |
| Panneau image | Ajuster, remplir ou étirer ; échelle, déplacement, rotation, miroir ; contraste, saturation, gamma, teinte et quatre courbes ; fusion normale, addition, écran et produit | Panneau *Picture* |
| Canevas et mapping | Plusieurs canevas sur une sortie, chacun déformé sur le mur par un maillage, et un ASC CDL par sortie pour accorder les projecteurs | *Show settings > Video* ; éditeur de déformation |
| NDI, Spout, Syphon | Envoyer une sortie à un autre programme, recevoir l'image d'un programme comme entrée live, ou faire passer l'image d'une cue par un autre programme et retour | *Show settings > Video* |
| Lecture anticipée | Les images fixes et films du prochain GO sont chargés avant l'appui, et chaque ligne de la liste dit si sa cue est prête | Liste de cues |
| DCA sur les images | Un DCA agit sur les images comme opacité ; le potentiomètre au-dessus d'une tranche DCA règle la courbe d'arrivée de l'image et le décalage du son | Surfaces ; panneau virtuel |
| Moniteur vidéo | La cue choisie seule à l'écran pendant qu'on la règle | S'ouvre avec la bande ou le panneau image |

Les images sont dessinées par un processus à part que Go.dot surveille : un film lent ne peut pas retenir une cue son.

### Cues process : Pure Data à l'intérieur (nouveau, en finition)

Une **cue process** contient un patch Pure Data et le fait tourner tant que la cue tourne, au rythme du contrôle, jamais dans le chemin audio. Elle entend ce qui arrive (retours des appareils, OSC, MIDI, lignes série) et envoie aux appareils, en MIDI et aux commandes de Go.dot. On édite le patch sur le canevas de Go.dot en pied de fenêtre, où les valeurs bougent en direct et où toggles, bangs, sliders et boîtes de nombre se jouent, ou on l'ouvre dans plugdata ou Pure Data : chaque sauvegarde revient. Des abstractions toutes faites couvrent les besognes courantes : `go.avg`, `go.minmax`, `go.smooth`, `go.scale`, `go.deadband`, `go.change`, `go.edge`, `go.hold` et `go.ratelimit`, chacune avec son patch d'aide, plus un spectacle d'exemple. *Add > process*.

### Importer des spectacles QLab et Ableton Live (nouveau)

- **QLab** : File > Import QLab workspace… lit directement le fichier d'un espace de travail QLab 4 ou 5, sans QLab. Cochez les listes de cues voulues ; Go.dot écrit un nouveau spectacle (cues, groupes, niveaux, fondus, routage et cibles) et un rapport de ce qu'il n'a pas pu reprendre.
- **Ableton Live** : l'import décompresse le fichier .als (un document XML gzippé) et lit le XML qu'il contient pour traduire le set Live en cues et groupes, pour faire passer à la liste de cues un spectacle construit en session. Une scène devient un GO, et les clips muets « track in » deviennent les fondus qu'ils représentent. L'import lit Live 10 à 12, fait d'une tournée un seul spectacle avec une représentation par lieu, et se trouve dans File > Import Ableton Live set…

### Protéger le spectacle

- **Historique d'annulation** de chaque modification (Ctrl+Maj+U), et sauvegarde automatique continue.
- **Verrouiller** le spectacle pendant la représentation (Ctrl+L) : GO, Échap et Doh! fonctionnent, l'édition non.
- **Spectacle et représentations.** Le spectacle est l'œuvre ; chaque soir est une représentation, une copie que l'on modifie sans toucher l'original. *File > New performance…*. À la fin, Go.dot liste ce qui a changé, cue par cue, pour reporter les bonnes modifications dans le spectacle.
- **Problèmes d'interface.** Si l'interface audio disparaît, le spectacle se met en pause et reprend à son retour. Si sa fréquence d'échantillonnage change, Go.dot suit.
- **Double-clic sur un `.wfg`** pour ouvrir le spectacle (installateur Windows, .deb Linux, macOS).

## Ce qui vient très bientôt

Ces fonctions sont conçues dans la spécification mais pas encore construites. L'ordre peut changer selon ce que demandent les testeurs : raison de plus pour envoyer vos retours.

### Application tablette et télécommande HTML

La console web tourne déjà dans n'importe quel navigateur du réseau. Elle deviendra une vraie **télécommande** : un panneau de conduite pour couper, avancer et déplacer ce qui joue ; des réglages à portée de main en arpentant la salle ; un GO depuis la place du concepteur ; et un secours complet si une surface de contrôle lâche. Les contrôles tactiles partent de l'endroit où le doigt se pose, donc rien ne saute, et les actions dangereuses demandent un geste délibéré plutôt qu'un simple appui. Une **application tablette native** viendra par-dessus, pour une raison : elle se réveille instantanément quand on la prend en pleine cue.

### Compensation de latence vidéo

On règle une fois le retard du vidéoprojecteur, et le son est retardé d'autant pour s'aligner sur l'image.

### Timecode

**LTC et MTC, en lecture et en génération.** Go.dot pourra suivre une source de timecode ou en être une.

### Enchaîner les membres d'une séquence

**Gap, gapless ou crossfade**, réglé une fois sur un groupe séquentiel : un blanc entre ses membres, aucun blanc, ou un fondu enchaîné dont vous choisissez la durée.

### Intégration logicielle

| Système | Ce que Go.dot en fera |
| --- | --- |
| **WFS-DIY** (synthèse de front d'onde) | Piloter ses sources, positions et LFO, réservés par cue pour que deux cues ne se disputent jamais une entrée |
| **XOA / Tight-WFS** | La même chose, pour le rendu ambisonique et WFS |
| **S21-HiJack** | Le piloter comme compagnon de la DiGiCo S21, sous sa propre branche d'adresses |
| **Choufleur** (suivi de texte) | Afficher la position dans le texte et les cues à venir à côté de la liste. Choufleur ne déclenche jamais rien, par principe |
| Processeurs **ADM-OSC** | Modèle intégré pour tout processeur de spatialisation objet qui parle ADM-OSC |
| **Millumin** | La porte de sortie pour les spectacles dont la vidéo dépasse Go.dot |
| Tout appareil OSCQuery | Découvert automatiquement, avec ses paramètres listés et vérifiés |

WFS-DIY, XOA/Tight-WFS, S21-HiJack et Choufleur viennent du même auteur que Go.dot. Ils restent des applications séparées, éventuellement sur d'autres machines ; Go.dot les dirige par le réseau.

### Intégration matérielle

- **Consoles son** : rappel de snapshots, mouvements de faders et de tranches, retour arrière Doh!. Un profil DiGiCo passe en premier, puis Yamaha, Allen&Heath et Behringer/Midas ; toute console qui parle OSC ou MIDI fonctionne déjà comme appareil opaque.
- **Consoles lumière** : GO et rappel de cues par OSC et MIDI, et sACN comme source entrante.
- **Surfaces de contrôle** : Mackie Control en v1 ; HUI, Icon, Behringer, PreSonus et Stream Deck ensuite. Une SpaceMouse fait déjà bouger les courbes des cues OSC.
- **Modèles d'appareils** : une bibliothèque partagée de descriptions, écrites par les utilisateurs, pour les consoles et processeurs qui ne se décrivent pas eux-mêmes.

## Faire un retour

Une version de test existe pour que d'autres que l'auteur disent ce qu'ils y ont trouvé. Chaque retour aide, y compris « je n'ai pas compris comment… ».

**Où :** [github.com/pob31/go.dot/issues](https://github.com/pob31/go.dot/issues). Un compte GitHub gratuit suffit. Écrivez en français ou en anglais.

**Un rapport de bug est le plus utile avec :**

1. Le nom de la version (le nom du fichier téléchargé, ou la première ligne de `wfg --version`).
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

**Les contributions de code** sont bienvenues aussi. Les instructions de compilation et les conventions du dépôt sont dans le README, sections *Building* et *Contributing*. Go.dot est sous GPL-3 : ce que vous apportez reste libre.
