# MaxPS4 0.5.0 — deuxième jeu jouable

**Mortal Kombat: Legacy Kollection tourne maintenant à 60 images par seconde sur iPhone 15 Pro Max, et Sonic Mania marche toujours très bien.**

C'est encore une **préversion expérimentale** : attends-toi à des bugs, et à ce que la plupart des jeux ne démarrent pas.

💬 Retours, questions et tests : **[Discord MaxPS4](https://discord.gg/G7BaXN4fd)**

## Nouveautés

- **Mortal Kombat: Legacy Kollection** jouable, avec le son : 60 images/s en combat, environ 54 quand le téléphone chauffe.
- **GPU beaucoup plus rapide** : les textures sont maintenant filtrées par le GPU de l'iPhone dans des cas qui passaient par une méthode logicielle lente. Sur Mortal Kombat, le temps GPU par image est passé de 47,5 ms à 13,8 ms.
- **Changer de jeu sans attendre** : quitter un jeu pouvait bloquer le suivant plus de 22 secondes.
- **Interruptions** : le jeu se met en pause au lieu de s'arrêter, par exemple quand tu ouvres le centre de contrôle.
- **Écran 120 Hz, Mode Jeu d'iOS** et **flèches tactiles** en vrai pavé directionnel, avec glissements et diagonales.

## Installer

1. Télécharge **MaxPS4-FEXCore-SideStore.ipa** ci-dessous et installe-la avec **SideStore**.
2. Ouvre MaxPS4, touche **JIT** et active le JIT avec **StikDebug** ([protocole détaillé](https://github.com/MaxFRdev/MaxPS4/blob/main/docs/fexcore-stikdebug-jit.md)). Le JIT est indispensable.
3. Copie le **dossier complet** de ton jeu (`eboot.bin`, `sce_module`, `sce_sys` et ses données) dans *Sur mon iPhone › MaxPS4 › Jeux*, puis touche **Jouer**.

## Limites

- **Seuls Sonic Mania et Mortal Kombat: Legacy Kollection sont confirmés jouables**, sur iPhone 15 Pro Max.
- **Pas encore de test de profondeur ni de culling** : les jeux en 3D ne s'affichent pas correctement pour l'instant.
- **Crash Bandicoot 4 est bloqué**, comme les autres jeux Unreal Engine 4 : iOS refuse les adresses mémoire fixes dont ils ont besoin.
- **La chaleur** fait baisser la cadence sur de longues sessions.
- **Aucun jeu n'est fourni et MaxPS4 ne déchiffre rien** : il faut le dossier déjà extrait d'un jeu que tu possèdes.
- Le JIT via StikDebug doit être réactivé à chaque lancement de l'app.
