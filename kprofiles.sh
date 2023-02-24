#!/bin/bash
#
# Import kprofiles for import and update

read -p "Please input Kprofiles branch name: " branch
read -p "Choose your options to do (import (i) or update (u)): " option

case $option in
    import | i)
        git subtree add --prefix=drivers/misc/kprofiles https://github.com/dakkshesh07/Kprofiles $branch
        echo "Done."
        ;;
    update | u)
        git subtree pull --prefix=drivers/misc/kprofiles https://github.com/dakkshesh07/kprofiles $branch
        echo "Done."
        ;;
    *)
        echo "Invalid character"
        ;;
esac

