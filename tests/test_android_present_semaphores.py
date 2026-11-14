"""A non-FIFO image acquisition reproduces unsafe frame-index semaphore reuse."""
from pathlib import Path
import unittest


class PresentSemaphores(unittest.TestCase):
    def test_non_fifo_reacquisition(self):
        def safe(per_image):
            pending={}
            for frame,image in enumerate((0,1,0,2,1,0,2,0)):
                pending={key:value for key,value in pending.items() if value!=image}
                semaphore=image if per_image else frame%3
                if semaphore in pending:
                    return False
                pending[semaphore]=image
            return True
        self.assertFalse(safe(False))
        self.assertTrue(safe(True))

    def test_production_uses_acquired_image(self):
        root=Path(__file__).resolve().parents[1]
        text=(root/'build/dxvk-source/src/dxvk/dxvk_presenter.cpp').read_text()
        self.assertEqual(text.count('sync.present = m_semaphores.at(m_imageIndex).present;'),2)
        self.assertIn('frostpunk_dxvk_surface_state(uint64_t(m_surface), nullptr, 0, 1)',text)


if __name__=='__main__':unittest.main()
