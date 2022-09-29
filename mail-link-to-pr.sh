#!/bin/bash
# Update the various branches and tags

if [ -n "$1" ]; then
    echo "usage: $0 <link to lore.kernel.org>"
fi

mkdir -p ./temp; cd ./temp

b4 am $1 >b4-output

git checkout -b pr-xilinx-remoteproc-v5 remoteproc/for-next
git am v5_20220518_tanmay_shah_add_xilinx_rpu_subsystem_support.mbx
git push wam
gh pr create --title xilinx-remoteproc-v5 \
    --body-file ./v5_20220518_tanmay_shah_add_xilinx_rpu_subsystem_support.cover \
    --draft --base remoteproc/for-next


echo "This PR is read-only and for the CI system.  DO NOT ADD COMMENTS HERE!" >header
echo "To add comments, please respond on the remoteproc mail list" >>header
echo "To reply, use the link below and follow the reply instructions at the end of the page" >>header
echo "Link: https://lore.kernel.org/r/20220520082940.2984914-1-arnaud.pouliquen@foss.st.com" >>header
echo "----------------------------------------------------------------" >>header
echo "" >>header
cat header 20220520_arnaud_pouliquen_introduction_of_rpmsg_flow_control_service.cover >pr-body
gh pr create \
    --title rpmsg-flow-control-service-rfc-v1  \
    --body-file pr-body --draft --base remoteproc/for-next
rm *.mbx *.cover header pr-body
