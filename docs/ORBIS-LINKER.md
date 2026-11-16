# Linker Orbis expérimental

Le linker prépare un ELF Orbis statique ou plusieurs fichiers ELF/PRX Orbis autonomes en mémoire
invitée isolée. Il ne lance aucun jeu. Il ne lit pas le PFS, ne déchiffre aucun
conteneur et ne possède aucune clé, licence ni table NID Sony.

## Audit et séparation des responsabilités

- `PS4ExecutableImage.hpp` conserve le parsing ELF/SELF plaintext et le staging
  des segments (32 Mio/module). Ses validations existantes sont réutilisées.
- `PS4PKGRuntimePlan.hpp` conserve l'inventaire et la validation des dépendances
  explicites (128 modules, 64 Mio/batch). Il ne change pas de comportement.
- `OrbisLinker.hpp` lit les métadonnées dynamiques SCE, indexe les exports,
  résout les imports et prépare toutes les écritures avant de les appliquer.
- `OrbisLinkerBridge.h/.cpp` expose un handle opaque, les cartographies invitées,
  les points d'entrée et des lectures exactes au code natif/Swift. Le pont est
  compilé dans l'application iOS par `project.yml`.
- `maxps4_guest_elf` et `maxps4_fex_runtime` conservent leur admission CPU ELF
  synthétique restrictive. Leur staging PS4 ne devient pas une exécution PS4.

Les cinq workflows associés au commit initial `dd18617` de la PR #173 ont
terminé avec succès (PKG, smoke, heavy, liaison FEX et IPA). Leur état initial
`queued` ne constituait pas une régression du code.

## Sous-ensemble vérifié

Les constantes et champs SCE ont été vérifiés contre les fichiers
[elf.h](https://github.com/shadps4-emu/shadPS4/blob/443f022450f6857256e780daa3d5a14029a748f7/src/core/loader/elf.h),
[module.h](https://github.com/shadps4-emu/shadPS4/blob/443f022450f6857256e780daa3d5a14029a748f7/src/core/module.h)
et [module.cpp](https://github.com/shadps4-emu/shadPS4/blob/443f022450f6857256e780daa3d5a14029a748f7/src/core/module.cpp).
L'implémentation bornée est propre à MaxPS4 : elle ne réutilise pas leurs accès
par pointeurs non bornés, leurs stubs d'import inconnu ou leur exécution.

- Un `PT_DYNAMIC` avec terminateur `DT_NULL` et un `PT_SCE_DYNLIBDATA` requis.
  Les offsets SCE sont relatifs au contenu de DYNLIBDATA, pas des pointeurs hôte.
- Tables SCE STRTAB/STRSZ, SYMTAB/SYMENT/SYMTABSZ et RELA/RELASZ/RELAENT,
  ainsi que JMPREL/PLTRELSZ/PLTREL (`DT_RELA = 7`). Entrées ELF64 de 24 octets.
- `DT_SCE_MODULE_INFO`, `NEEDED_MODULE`, `EXPORT_LIB` et `IMPORT_LIB` : offset
  chaîne sur 32 bits, version sur 16 bits, identifiant local sur 16 bits.
  Pour les modules, la version conserve les octets minor/major du format.
- NID encodé de 11 caractères, suivi de `#libraryID#moduleID`. Les identifiants
  locaux utilisent l'alphabet Orbis `A-Z a-z 0-9 + -`. Ils servent à lire les
  descripteurs ; ils ne constituent pas l'identité intermodule.
- Résolution exacte sur NID, noms et versions de bibliothèque/module, type de
  symbole. Aucun hash de nom de fonction, NID réel inventé ou alias automatique.
- Symboles locaux, globaux et faibles de types NOTYPE/OBJECT/FUNC, visibilité
  par défaut, SHN_UNDEF ou indice ordinaire. Les imports faibles manquants sont
  refusés, comme les imports globaux. Les exports dupliqués sont refusés même
  faibles ; aucune priorité dépendant de l'ordre d'entrée.
- DT_NEEDED avec offset dans la table SCE de chaînes, vérification des descripteurs
  module/bibliothèque même inutilisés : voir [Orbis Runtime Foundation](orbis-runtime-foundation.md).
- Dépendances fournies explicitement par le demandeur : un fournisseur d'import
  doit être déclaré et chargé avant le consommateur. Graphe fermé, sans cycles,
  chemins dupliqués ou dépendances dupliquées. Tri lexical stable des racines et
  dépendances avant la validation existante ; ordre indépendant de l'entrée.

Formules [AMD64 System V ABI, relocations](https://gitlab.com/x86-psABIs/x86-64-ABI):

| Type | Valeur écrite sur 64 bits |
| --- | --- |
| R_X86_64_64 (1) | S + A |
| R_X86_64_GLOB_DAT (6) | S |
| R_X86_64_JUMP_SLOT (7) | S |
| R_X86_64_RELATIVE (8) | B + A, symbole nul requis |
| R_X86_64_DTPMOD64 (16) | ID du module TLS qui définit S (symbole nul : module courant) |
| R_X86_64_DTPOFF64 (17) | S + A, décalage dans le bloc TLS du module |
| R_X86_64_TPOFF64 (18) | S + A − décalage du bloc (valeur négative, variante II) |

`S` est l'adresse invitée du symbole, `A` l'addend signé explicite RELA, `B` le
biais invité choisi par le demandeur. Les dépassements et sous-dépassements sont
refusés, sans wraparound, sauf la valeur volontairement négative de TPOFF64.
Le biais ne réécrit pas les métadonnées module-locales.
Aucun type inconnu (COPY, IFUNC, PC32, REL, TLS dynamique…) n'est silencieusement
ignoré.

### TLS statique

Un module peut avoir un seul `PT_TLS` : alignement puissance de deux ≤ 4096,
`p_filesz ≤ p_memsz`, 1 Mio au plus, image d'initialisation dans des octets
chargés, lisibles et présents dans le fichier. Les modules TLS reçoivent les ID
1, 2… dans l'ordre de chargement. Disposition x86-64 « variante II »
(FreeBSD/Orbis) : chaque bloc se termine sous le pointeur de thread, à
`TP − offset`, avec `offset = arrondi(offset précédent + p_memsz, alignement)`.
`Image::tls` expose ID, adresse invitée de l'image, tailles, alignement et
offset ; `staticTLSSize` est la taille totale par thread (1 Mio au plus).

Les symboles `STT_TLS` portent un décalage dans le bloc de leur module, borné
par `p_memsz`. Une relocation TLS sur un symbole non TLS (ou l'inverse), un
décalage hors bloc ou une relocation TLS dans un module sans `PT_TLS` sont refusés.
L'alignement de l'adresse de l'image (`p_vaddr % p_align`) n'est pas encore
intégré au calcul de l'offset.

Exécution (chemin borné `GuestELF::prepare_orbis` + FEX) : une région RW, non
exécutable, est créée après une page de garde. Elle contient les blocs, alignés
sur au moins 16 octets et initialisés depuis les images TLS (reste à zéro), puis
un TCB de 16 octets au pointeur de thread, dont le mot 0 contient sa propre
adresse hôte. FEX reçoit cette adresse dans `fs_cached`. Formes d'accès admises,
toujours si les 4 octets lus sont dans les blocs ou le TCB :

| Modèle | Séquence |
| --- | --- |
| local-exec direct | `MOV EAX, FS:[disp32]` |
| local-exec via registre | `MOV RAX, FS:[0]` puis, immédiatement, `MOV EAX, [RAX+disp8/32]` |
| initial-exec | `MOV RAX, [RIP+GOT]` puis, immédiatement, `MOV EAX, FS:[RAX]` |

Pour initial-exec, le slot GOT (rempli par TPOFF64) doit être en lecture seule à
l'exécution, c'est-à-dire dans un segment SCE RELRO : aucune écriture admise ne
peut alors modifier le décalage.

General-dynamic : `LEA RDI,[RIP+tls_index]` (rempli par DTPMOD64/DTPOFF64), puis
`CALL [GOT]` vers la trampoline HLE de `__tls_get_addr` (NID `vNe1w4diLCs`,
libkernel v1, d'après shadPS4), puis immédiatement `MOV EAX,[RAX]`. `tls_index`
étant modifiable par le jeu, le gate FEX le relit et le revalide à chaque appel
(module existant, décalage < `p_memsz`). Il ne renvoie jamais de pointeur
invalide : en cas d'échec, il renvoie le TCB et l'exécution échoue avec
`-EFAULT`. L'admission refuse l'appel si l'image n'a aucun bloc TLS.
Limites actuelles du chemin borné : un seul thread, pas d'écriture TLS (le mode
général a des threads, voir plus bas). Les autres tags dynamiques, même certains tags standard usuels, sont
refusés tant que leur sémantique n'est pas implémentée et testée. Ce sous-ensemble
ne prétend donc pas accepter arbitrairement tous les PRX réels ou les ELF Linux.

### Démarrage dynamique

`DT_INIT`, `DT_FINI`, `DT_INIT_ARRAY(SZ)` et `DT_FINI_ARRAY(SZ)` sont acceptés.
Chaque fonction doit être du code présent dans le fichier du même module ; les
tableaux (taille multiple de 8, 256 entrées au plus) sont lus **après** les
relocations, et les sentinelles 0 et −1 sont ignorées.

Ce qui s'exécute suit shadPS4 (`Module::Start`) : sur PS4, le `DT_INIT` d'un
PRX **est** son `module_start(args, argp, param)`, et c'est lui qui exécute son
`DT_INIT_ARRAY`. Un exécutable, lui, exécute ses propres initialiseurs depuis
son point d'entrée. `Image::initializers` contient donc uniquement le `DT_INIT`
de chaque PRX (0xfe18), dans l'ordre de chargement (dépendances d'abord).
`prepare_orbis` génère un stub RX après une page de garde : pour chaque PRX
`XOR EDI,EDI ; XOR ESI,ESI ; XOR EDX,EDX ; CALL module_start` — soit
`module_start(0, NULL, NULL)` comme `LoadAndStartModule` — puis `JMP entrée`.
L'admission commence par ce stub, dans le même budget. FEX démarre au stub
(`GuestELF::start()`) ; `entry()` reste le point d'entrée du module. Les
`DT_FINI*` sont seulement validés : rien ne s'exécute à la sortie. Dans le
chemin d'exécution général utilisé par les jeux, le stub vérifie maintenant le
retour de chaque `module_start` : zéro continue l'initialisation, une valeur
non nulle termine le processus avec ce statut avant son point d'entrée. Le
chemin borné de démonstration n'a pas de service `process_exit` et conserve son
ancienne séquence; il n'est pas celui utilisé par le lancement de jeu.

### Exécution générale (adressage identité, expérimental)

Le chemin borné prouve chaque instruction parce que FEX exécute à des adresses
hôte rebasées alors que les pointeurs absolus (RELATIVE, GOT, TLS) contiennent
des adresses PS4 logiques. `GuestELF::prepare_orbis_general` supprime cette
divergence : les adresses PS4 **sont** les adresses hôte.

1. Une première liaison mesure l'image (modules et trampolines HLE).
2. Une réservation `PROT_NONE` couvre cette étendue, plus le stub de démarrage,
   le TLS statique et une pile, chacun après une page de garde (256 Mio au plus).
3. Une seconde liaison déplace chaque `loadBias` de `relocation`, multiple de
   1 Mio, ce qui préserve les alignements de segments et du TLS, vers la
   réservation. Toutes les relocations visent donc directement des adresses hôte.
4. Les segments sont copiés à leur propre adresse ; `translate` est l'identité.
   Une pile RW de 1 Mio, encadrée de gardes, appartient à l'image : un pointeur
   vers un tampon de pile est un argument HLE valide.

Aucune liste d'instructions n'est appliquée et rien n'est réécrit : `CALL [GOT]`
s'exécute tel quel, comme les branchements, boucles, PUSH/POP, appels locaux et
déréférencements de pointeurs absolus. Un `ET_SCE_EXEC` (adresses fixes, non
relogeable) est refusé : l'adresse 0x400000 tombe dans `__PAGEZERO` sur iOS.

API C : `maxps4_fex_engine_prepare_orbis_general`, puis
`maxps4_fex_engine_run_orbis`. Tests FEX : boucle 1..10, appel local avec cadre,
pointeur RELATIVE, appel GOT non patché vers un PRX, store BSS (résultat
110 + cellule + valeur du PRX, deux valeurs différentes) et
`sceKernelClockGettime` écrivant sur la pile invitée.

**Fautes confinées (hôte ARM64).** Pendant `ExecuteThread`, un gestionnaire
`SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGTRAP` reconnaît une faute levée par le code
généré par FEX (`IsAddressInCodeBuffer`) ou par les stubs de signal invité du
dispatcher. Il remet la pile hôte sur `ReturningStackLocation` et place le PC
sur le gestionnaire d'arrêt de thread de FEX (`ThreadStopHandlerAddressSpillSRA`
depuis le code JIT, qui sauvegarde d'abord les registres invités) :
`ExecuteThread` revient normalement. L'exécution renvoie `-EFAULT` (accès
mémoire) ou `-EILSEQ` (instruction invalide) et
`maxps4_fex_engine_last_guest_fault` donne le signal, l'adresse fautive et le
RIP invité (à la granularité du bloc). Toute autre faute est transmise au
gestionnaire précédent. Tests ARM64 natif : lecture de la page de garde
au-dessus de la pile, écriture dans le code, `UD2`, puis exécution normale sur
le même moteur.

Limites actuelles, à traiter avant tout code non fiable :

- **Fautes encore fatales** : une faute dans le code C++ de FEX ou d'un HLE, et
  toute faute sous le simulateur VIXL (job x86), termine le processus. Sur
  iPhone, un débogueur attaché (JIT via StikDebug) peut intercepter l'exception
  Mach avant le signal. L'application ne propose donc pas encore ce mode.
- **Limite de temps partielle** : `maxps4_fex_engine_set_time_limit` borne
  chaque exécution (ARM64 natif). À l'échéance, un chien de garde envoie
  `SIGURG` au thread toutes les millisecondes ; reçu dans le code généré ou le
  dispatcher, il arrête le thread comme une faute (`-ETIMEDOUT`). Un appel long
  dans le C++ de FEX ou d'un HLE n'est pas interrompu : l'arrêt a lieu à la
  reprise du code invité. Test : `JMP $` arrêté après 200 ms.
- **`sceKernelMmap` en mémoire hôte** : une fenêtre de 32 Mio suit la pile
  (après une page de garde) dans la réservation, `PROT_NONE` jusqu'à
  l'allocation. `PS4VirtualMemory` y place les mappings anonymes à leur propre
  adresse et applique `mprotect` ; `munmap` remplace les pages (remises à zéro).
  Le code invité déréférence donc directement le pointeur rendu. Le septième
  argument (sur la pile) est lu à `RSP+8` pour les images générales. Les pages
  exécutables restent de la donnée hôte : FEX ne les exécute pas encore.
- **Entrée de processus PS4** : en mode général, le stub de démarrage existe
  toujours. Après les `module_start` (pile alignée), il reproduit
  `RunMainEntry` de shadPS4 : `SUB RSP,8`, RDI = `&EntryParams` (haut de la
  pile invitée : argc 1, argv[0] `"eboot.bin"`, réécrits à chaque exécution),
  copie de leurs 16 premiers octets en `[RSP]`, RSI = fonction de sortie, puis
  `JMP entry`. La fonction de sortie est une trampoline du moteur (service
  `process_exit`, sans NID) : son appel termine l'exécution, RAX = statut, et
  le rapport CPU indique `exited`/`exit_status`.
- **Threads invités** (mode général) : `scePthreadCreate` (`6UgtwV+0zb4`),
  `scePthreadJoin` (`onNY9Byn-W8`) et `scePthreadExit` (`3kg7rT0NQIs`) sont
  servis par la porte HLE de FEX. Chaque thread est un vrai thread hôte avec
  son propre état FEX : RIP = entrée, RDI = argument, FS = son TCB. Il tourne
  dans l'un des 8 emplacements placés après la fenêtre mmap. Un emplacement
  contient une pile de 1 Mio suivie d'une copie privée du TLS statique, entre
  pages de garde. `[RSP]` (RSP = 8 mod 16) contient une trampoline de retour
  du moteur (`MOV RDI,RAX` ; service `thread_exit` ; `HLT`). Revenir de
  l'entrée ou appeler `scePthreadExit` termine donc le thread, et `join` rend
  sa valeur. Le handle écrit en `*thread` est l'identité PS4 du thread, celle
  que rend `scePthreadSelf` dans ce thread. L'objet d'attributs donne l'état
  détaché (un thread détaché, ou passé à `scePthreadDetach`, rend son
  emplacement à sa fin) et la taille de pile, qui doit tenir dans le 1 Mio de
  l'emplacement (sinon EAGAIN) ; priorité et affinité ne sont pas appliquées. FEX ne termine pas le
  bloc sur un SYSCALL (changer `State.rip` y est sans effet) : la trampoline
  de `scePthreadExit` finit donc par `HLT` au lieu de `RET`. Si l'entrée
  appelle `scePthreadExit`, le processus continue jusqu'à la fin de ses autres
  threads, puis sort avec le statut 0. Quand l'entrée se termine autrement, les
  threads restants sont arrêtés (`SIGURG`, comme le chien de garde) puis
  joints. Un thread qui appelle la fonction de sortie de l'entrée termine le
  processus : le premier statut l'emporte et le thread principal est arrêté.
  Une faute confinée dans un thread arrête aussi le thread principal et devient
  le résultat de l'exécution. Ces trois services ne sont résolus que pour les
  images générales : une image bornée qui les importe est refusée. Sous le
  simulateur VIXL (CI x86), `scripts/patch-fexcore-simulator-threads.py` donne
  à chaque thread hôte son propre simulateur et sérialise les instructions
  atomiques ; une faute n'y est pas confinée et un thread ne peut pas y être
  arrêté de force : la fin d'exécution attend que chaque thread se termine.
- Pas de SYSCALL hors trampoline (l'exécution échoue avec `-EPERM`).
- **Conteneur SELF** (`SelfImage.hpp`, disposition du chargeur de shadPS4
  `core/loader/elf.cpp`) : un SELF dont les blocs sont en clair (fSELF des
  homebrews, dump déchiffré sur la console) est lu comme son ELF. Chaque bloc
  « blocked » d'ID n contient les octets du segment ELF n ; ils sont replacés
  à leur position ELF, et les segments sans bloc doivent tenir dans un autre.
  L'analyse, le précontrôle et le chargeur FEX passent par là. Un SELF à blocs
  chiffrés (0x2) ou compressés (0x8) est refusé avec un message clair :
  MaxPS4 ne déchiffre ni ne décompresse rien.
- **Jeux du commerce** : un premier vrai jeu (exécutable en SELF à blocs en
  clair, 274 imports sur 17 bibliothèques, 1457 relocations) se lie
  entièrement. Pour cela : relocations dans `PT_SCE_RELRO`, `PT_TLS` vide,
  en-têtes de métadonnées (`PT_INTERP`, paramètres SCE, `PT_GNU_*`,
  commentaire, version de bibliothèque), tags dynamiques informatifs,
  `DT_PREINIT_ARRAY`, tableaux d'init/fini vides, attributs de module et de
  bibliothèque répétés, symboles locaux de section, modules système déclarés
  mais inutilisés ou servis par la HLE sans fichier PRX. Les fonctions HLE
  correspondent à toute version de module (comme dans shadPS4), et un import
  de données (`STT_OBJECT`) reçoit des octets RW à zéro dans le module
  `<hle-data>` (`Image::data`).
- **Analyse sur PC** : `MaxPS4-iOS/Tools/ps4-orbis-analyze.cpp` fait la même
  analyse que l'app, puis une liaison où chaque import reçoit une trampoline
  ou une donnée provisoire : elle vérifie tout le chargeur sans dépendre des
  fonctions implémentées. Lecture seule, aucune exécution.

## Transactions et contrat mémoire/FEX

`OrbisLinker::link(inputs, hle, output, diagnostic, budget)` conserve intégralement
`output` sur échec. L'image provisoire est détruite. Les fichiers source sont
immuables ; aucun pointeur mutable n'est transmis au backend. La construction
vérifie le budget complet des segments avant toute allocation de staging.
Les adresses de tous les segments sont vérifiées contre les dépassements et les
chevauchements intermodules. Les cibles de relocation doivent contenir huit
bytes dans un segment RW et non X. Les écritures qui se chevauchent sont refusées.
Toutes les relocations sont validées avant la première écriture dans l'image
provisoire ; les segments SCE RELRO deviennent read-only après ces écritures.

Le budget est plafonné à 64 Mio pour les segments invités, avec 32 Mio/module.
Il ne mesure pas toute la consommation du processus : l'image précédente reste
vivante pendant la transaction et les index/métadonnées nécessitent de la mémoire
supplémentaire, bornée séparément. Au plus 65 536 symboles et 65 536 relocations
pour le batch, 4 096 entrées dynamiques/module, 1 Mio de chaînes/module et chaînes
identitaires limitées à 255 caractères. Un échec d'allocation est géré.

`Image::mappings()` fournit adresse invitée, taille, permissions ELF PF_R/W/X et
pointeur **const** vers les données appartenant à l'image. Le pont C expose les
mêmes informations sans adresse hôte, puis `maxps4_orbis_read` copie des octets.
Le backend doit créer sa propre cartographie à ces adresses logiques et appliquer
les permissions. Les bytes hôte sont des vecteurs ordinaires, jamais PROT_EXEC.
Les points d'entrée non nuls doivent désigner du code file-backed ; un PRX peut
ne pas avoir d'entrée. Les consultations échouées ne modifient pas les sorties.

**Trampolines HLE synthétisées.** Une entrée HLE avec `guestAddress = 0` et un
`token` non nul demande au linker d'écrire la trampoline
`MOV R10,RCX ; MOV EAX,token ; SYSCALL ; RET` (11 octets) dans un module `<hle>`
RX, placé après tous les modules et une garde de 16 Kio (`Image::thunks`).
Une bibliothèque/un module importé que seul le registre HLE fournit (même nom
et même version) n'exige pas de fichier PRX ni de dépendance déclarée ; si un
module fourni porte le même nom, la règle explicite habituelle s'applique.
`maxps4_fex_engine_prepare_orbis` enregistre ainsi chaque NID implémenté de
`libkernel` (identité libkernel/libkernel, versions 1 — à confirmer sur des
binaires réels). Côté exécution, un `CALL [GOT]` vers une trampoline est admis
sans admettre son `SYSCALL` comme code invité ; FEX n'appelle `PS4Libkernel`
que si le `SYSCALL` est exactement celui d'une trampoline avec le même jeton.
Arguments SysV (RDI, RSI, RDX, R10, R8, R9) ; pas de 7ᵉ argument sur la pile.
Arguments admis : `MOV EDI/ESI, imm32` et `LEA RDI/RSI/RDX, [RIP+rel32]` vers
des données invitées non exécutables. FEX exécutant à des adresses hôte
rebasées, chaque argument situé dans l'arène de l'image est retraduit en
adresse PS4 logique avant l'appel ; libkernel revérifie ensuite permissions et
longueurs (ex. `sceKernelClockGettime` écrit son `timespec` en mémoire invitée).

Le registre HLE C++ est explicite et vide par défaut. Une entrée contient
l'identité exacte et une adresse de trampoline **invité** déjà cartographié RX.
Un pointeur de fonction hôte n'est pas un binding HLE. Les collisions HLE/export,
versions incompatibles et adresses de données sont refusées. Aucun trampoline
Orbis réel ni appel hôte n'est fourni ici. La fixture HLE teste ce contrat, pas
une implémentation de libkernel.

Le pont natif ne permet aucune exécution. Il reste à implémenter et tester un
backend d'espace d'adresses FEX pour ces cartographies, avec trampolines HLE et
ABI Orbis, TLS, pile, initialisation des modules et syscalls. Passer simplement
un pointeur de vecteur au RIP FEX serait incorrect et n'est pas proposé.

## Validation automatique

`OrbisLinkerFixture.h` fabrique deux ELF Orbis en mémoire ; ses identités
`FixtureLibrary`, `FixtureModule`, `AAAAAAAAAAA` sont des données de test sans
correspondance revendiquée avec un symbole Sony. `OrbisLinkerTest` peut écrire
ces deux fixtures dans un répertoire fourni en argument pour le test Swift.

Le workflow existant `pkg-inventory.yml` exécute sur push/PR les tests linker avec
ASan/UBSan sur Linux et macOS, les régressions PKG/SFO existantes, le véritable
appel Swift/C du linker et la compilation/typecheck iOS ARM64. Le workflow IPA
exécute aussi le test linker avant packaging et compile son pont dans l'app.
Les workflows complets de liaison FEX et smoke/heavy restent actifs.

Les tests couvrent import présent/absent/version incompatible, ambiguïtés,
cycles, dépendances absentes/non déclarées, ordre déterministe, les quatre
formules RELA et PLT RELA, erreurs d'indices/limites/types/addends, chevauchements,
budgets, permissions W+X/RX/R-only/RELRO, métadonnées tronquées/malformées,
transaction échouée après une première relocation valide, immutabilité de la
source, registre HLE et pont natif. Les fixtures PKG/SFO restent inchangées.

## Vers CUSA06015

Kero Blaster n'est pas exécutable dans MaxPS4 à ce stade. Son PFS reste opaque.
La prochaine étape technique est une cartographie FEX des deux modules
synthétiques liés, puis une exécution bornée traversant un trampoline invité
validé. Le chargement de Kero Blaster nécessitera ultérieurement une source
légalement accessible de modules plaintext et des imports/ABI réellement vérifiés.
Aucune extraction ou décryption du PFS protégé n'est une étape de ce travail.

La préparation et le diagnostic iOS renforcés sont décrits dans
[Orbis Runtime Foundation](orbis-runtime-foundation.md), avec
[les validations et limites de l’environnement](orbis-runtime-validation.md).
