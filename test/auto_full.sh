ISO=/home/user/Doubao/chats/38442617987459330/dueslin-build/iso/DUESLIN-1.0.iso
DISK=/tmp/dtestE.qcow2
rm -f "$DISK" /tmp/serialG.log /tmp/qmonG.sock /tmp/serialH.log /tmp/qmonH.sock /tmp/final3.ppm
qemu-img create -f qcow2 "$DISK" 20G 2>/dev/null
nohup qemu-system-x86_64 -m 2048 -smp 2 \
  -drive file="$DISK",if=virtio,format=qcow2 \
  -cdrom "$ISO" -boot d -vga std -display none \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -serial file:/tmp/serialG.log \
  -monitor unix:/tmp/qmonG.sock,server,nowait \
  -usb -device usb-tablet -no-reboot > /tmp/qemuG.log 2>&1 &
sleep 14
python3 /home/user/Doubao/chats/38442617987459330/dueslin-project/test/qemu_keys.py /tmp/qmonG.sock down ret
echo "MENU_SELECTED"
echo "WAIT_INSTALL"
for i in $(seq 1 90); do
  sleep 15
  if grep -qE "安装完成|准备重启|重启系统" /tmp/serialG.log 2>/dev/null; then
    echo "INSTALL_DONE_$((i*15))s"; break
  fi
done
pkill -9 -f qemu-system 2>/dev/null || true
sleep 2
echo "PHASE2_START"
nohup qemu-system-x86_64 -m 2048 -smp 2 \
  -drive file="$DISK",if=virtio,format=qcow2 \
  -vga std -display none \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -serial file:/tmp/serialH.log \
  -monitor unix:/tmp/qmonH.sock,server,nowait \
  -usb -device usb-tablet -no-reboot > /tmp/qemuH.log 2>&1 &
for i in $(seq 1 30); do
  sleep 10
  if grep -q "X.Org X Server" /tmp/serialH.log 2>/dev/null; then
    echo "X_STARTED_$((i*10))s"
    sleep 12
    break
  fi
done
python3 /home/user/Doubao/chats/38442617987459330/dueslin-project/test/qemu_shot.py /tmp/qmonH.sock /tmp/final3.ppm
pkill -9 -f qemu-system 2>/dev/null || true
echo "ALL_DONE"
